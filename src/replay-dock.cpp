/*
OBS Instant Replay - operator dock (Qt) implementation
*/

#include "replay-dock.hpp"
#include "replay-controller.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr const char *kDockId = "obs-instant-replay-dock";

std::string module_config_file(const char *file)
{
	char *p = obs_module_config_path(file);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

// Auto-refresh the scene checklist when OBS's scene list changes.
void dock_frontend_event(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<ReplayDock *>(data);
	if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED ||
	    event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Frontend events fire on the UI thread; safe to touch widgets, but
		// queue to be defensive against re-entrancy during load.
		QMetaObject::invokeMethod(dock, "reloadScenes", Qt::QueuedConnection);
	}
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// The Controls dock exists by now - safe to inject our button.
		QMetaObject::invokeMethod(dock, "ensureControlsButton", Qt::QueuedConnection);
	}
}

} // namespace

ReplayDock::ReplayDock(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	// --- Toggle buffer ---
	toggleBtn_ = new QPushButton(this);
	toggleBtn_->setMinimumHeight(40);
	connect(toggleBtn_, &QPushButton::clicked, this, &ReplayDock::onToggleClicked);
	root->addWidget(toggleBtn_);

	statusLabel_ = new QLabel(this);
	statusLabel_->setAlignment(Qt::AlignCenter);
	root->addWidget(statusLabel_);

	// --- Scene selection ---
	auto *group = new QGroupBox("Scenes to buffer (each = one replay angle)", this);
	auto *gl = new QVBoxLayout(group);
	sceneList_ = new QListWidget(group);
	sceneList_->setSelectionMode(QAbstractItemView::NoSelection);
	connect(sceneList_, &QListWidget::itemChanged, this, &ReplayDock::onSceneItemChanged);
	gl->addWidget(sceneList_);
	auto *refreshBtn = new QPushButton("Refresh scene list", group);
	connect(refreshBtn, &QPushButton::clicked, this, &ReplayDock::reloadScenes);
	gl->addWidget(refreshBtn);
	root->addWidget(group);

	// --- Replay angle buttons ---
	auto *anglesRow = new QHBoxLayout();
	for (int i = 0; i < 4; ++i) {
		angleBtns_[i] = new QPushButton(QString("Replay %1").arg(i + 1), this);
		connect(angleBtns_[i], &QPushButton::clicked, this, [this, i]() { onAngleClicked(i); });
		anglesRow->addWidget(angleBtns_[i]);
	}
	root->addLayout(anglesRow);

	returnBtn_ = new QPushButton("Return to Live", this);
	connect(returnBtn_, &QPushButton::clicked, this, &ReplayDock::onReturnClicked);
	root->addWidget(returnBtn_);

	root->addStretch(1);

	// Persisted selection -> controller, then populate the list.
	loadSelection();
	pushSelectionToController();
	reloadScenes();

	installStatusBarIndicator();

	// Keep the UI in sync with buffering state.
	timer_ = new QTimer(this);
	timer_->setInterval(500);
	connect(timer_, &QTimer::timeout, this, &ReplayDock::refresh);
	timer_->start();

	obs_frontend_add_event_callback(dock_frontend_event, this);
	ensureControlsButton(); // best-effort now; also retried on FINISHED_LOADING
	refresh();
}

ReplayDock::~ReplayDock()
{
	obs_frontend_remove_event_callback(dock_frontend_event, this);
	removeStatusBarIndicator();
	removeControlsButton();
}

std::vector<std::string> ReplayDock::checkedScenes() const
{
	std::vector<std::string> out;
	for (int i = 0; i < sceneList_->count(); ++i) {
		QListWidgetItem *it = sceneList_->item(i);
		if (it->checkState() == Qt::Checked)
			out.push_back(it->text().toUtf8().constData());
	}
	return out;
}

void ReplayDock::reloadScenes()
{
	updatingList_ = true;
	sceneList_->clear();

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		const char *name = obs_source_get_name(scenes.sources.array[i]);
		auto *item = new QListWidgetItem(QString::fromUtf8(name), sceneList_);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		bool checked = std::find(selection_.begin(), selection_.end(), std::string(name)) !=
			       selection_.end();
		item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
	}
	obs_frontend_source_list_free(&scenes);

	updatingList_ = false;
}

void ReplayDock::onSceneItemChanged()
{
	if (updatingList_)
		return;
	selection_ = checkedScenes();
	saveSelection();
	pushSelectionToController();
	// If buffering is already on, note that a re-toggle is needed to apply.
	refresh();
}

void ReplayDock::onToggleClicked()
{
	// Make sure the controller has the latest selection before it builds.
	selection_ = checkedScenes();
	pushSelectionToController();
	ReplayController::instance().toggle_buffering();
	// State flips on the UI task queued by toggle_buffering; refresh() (timer)
	// will catch up shortly.
}

void ReplayDock::onReturnClicked()
{
	ReplayController::instance().return_to_live();
}

void ReplayDock::onAngleClicked(int index)
{
	ReplayController::instance().take_replay((size_t)index, 8.0, 0.5, 0.50);
}

void ReplayDock::pushSelectionToController()
{
	ReplayController::instance().set_selected_scenes(selection_);
}

void ReplayDock::loadSelection()
{
	selection_.clear();
	obs_data_t *d = obs_data_create_from_json_file(module_config_file("selection.json").c_str());
	if (!d)
		return;
	obs_data_array_t *arr = obs_data_get_array(d, "scenes");
	size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		selection_.push_back(obs_data_get_string(item, "name"));
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(d);
}

void ReplayDock::saveSelection()
{
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *d = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const std::string &name : selection_) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "scenes", arr);
	obs_data_array_release(arr);
	obs_data_save_json(d, module_config_file("selection.json").c_str());
	obs_data_release(d);
}

void ReplayDock::installStatusBarIndicator()
{
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw || !mw->statusBar())
		return;
	statusBarLabel_ = new QLabel(mw->statusBar());
	statusBarLabel_->setText("Replay: OFF");
	mw->statusBar()->addPermanentWidget(statusBarLabel_);
}

void ReplayDock::removeStatusBarIndicator()
{
	if (!statusBarLabel_)
		return;
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (mw && mw->statusBar())
		mw->statusBar()->removeWidget(statusBarLabel_);
	delete statusBarLabel_;
	statusBarLabel_ = nullptr;
}

void ReplayDock::ensureControlsButton()
{
	if (controlsBtn_)
		return; // already injected
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw)
		return;
	// OBS's Controls dock buttons live in a QVBoxLayout named "buttonsVLayout".
	auto *lay = mw->findChild<QVBoxLayout *>("buttonsVLayout");
	if (!lay) {
		obs_log(LOG_WARNING, "replay: could not find Controls layout - button not added");
		return;
	}
	controlsBtn_ = new QPushButton(mw);
	controlsBtn_->setMinimumHeight(28);
	connect(controlsBtn_, &QPushButton::clicked, this, &ReplayDock::onToggleClicked);
	// Place it just above the Settings button if we can find it, else append.
	auto *settings = mw->findChild<QPushButton *>("settingsButton");
	int idx = settings ? lay->indexOf(settings) : -1;
	if (idx >= 0)
		lay->insertWidget(idx, controlsBtn_);
	else
		lay->addWidget(controlsBtn_);
	obs_log(LOG_INFO, "replay: added button to Controls dock");
	refresh();
}

void ReplayDock::removeControlsButton()
{
	if (controlsBtn_) {
		delete controlsBtn_; // removes it from OBS's Controls layout
		controlsBtn_ = nullptr;
	}
}

void ReplayDock::refresh()
{
	auto &ctrl = ReplayController::instance();
	const bool on = ctrl.is_buffering();
	const size_t angles = ctrl.channel_count();

	toggleBtn_->setText(on ? "Replay Buffer: ON  (click to stop)" : "Replay Buffer: OFF  (click to start)");
	toggleBtn_->setStyleSheet(on ? "background-color:#2d7d2d; color:white; font-weight:bold;"
				     : "background-color:#7d2d2d; color:white; font-weight:bold;");

	// The button injected into OBS's Controls dock (if present).
	if (controlsBtn_) {
		controlsBtn_->setText(on ? "Stop Instant Replay" : "Start Instant Replay");
		controlsBtn_->setStyleSheet(on ? "background-color:#2d7d2d; color:white;" : "");
	}

	if (on)
		statusLabel_->setText(QString("Buffering %1 angle(s) - last 30s").arg(angles));
	else
		statusLabel_->setText("Idle - not buffering");

	for (int i = 0; i < 4; ++i) {
		// Label each replay button with the actual scene name: the live channel
		// label when buffering, otherwise the i-th selected scene.
		QString name;
		if (on && (size_t)i < angles)
			name = QString::fromUtf8(ctrl.angle_label((size_t)i).c_str());
		else if ((size_t)i < selection_.size())
			name = QString::fromUtf8(selection_[i].c_str());
		else if (selection_.empty() && i == 0)
			name = "Program feed";

		const bool used = !name.isEmpty();
		angleBtns_[i]->setVisible(used); // hide buttons with no assigned scene
		angleBtns_[i]->setText(used ? ("▶ " + name) : QString("Replay %1").arg(i + 1));
		angleBtns_[i]->setEnabled(on && (size_t)i < angles);
	}
	returnBtn_->setEnabled(on);

	if (statusBarLabel_)
		statusBarLabel_->setText(on ? QString("● Replay: ON (%1)").arg(angles) : "○ Replay: OFF");
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static ReplayDock *g_dock = nullptr;

void register_replay_dock()
{
	if (g_dock)
		return;
	// Owned by OBS once added; removed/deleted by obs_frontend_remove_dock.
	g_dock = new ReplayDock();
	if (!obs_frontend_add_dock_by_id(kDockId, "Instant Replay", g_dock)) {
		obs_log(LOG_WARNING, "replay: failed to add dock");
		delete g_dock;
		g_dock = nullptr;
		return;
	}
	obs_log(LOG_INFO, "replay: operator dock added");
}

void unregister_replay_dock()
{
	if (!g_dock)
		return;
	obs_frontend_remove_dock(kDockId); // deletes the dock and our widget
	g_dock = nullptr;
}
