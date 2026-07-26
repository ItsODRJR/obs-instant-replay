/*
OBS Instant Replay - operator dock (Qt)

A dockable panel with:
  * a big Toggle Buffer button (green when buffering)
  * a status line
  * a checklist of scenes to buffer as camera angles
  * per-angle "Replay" buttons and a Return-to-Live button
Plus a small indicator added to OBS's status bar.
*/

#pragma once

#include <QWidget>

#include <string>
#include <vector>

QT_BEGIN_NAMESPACE
class QPushButton;
class QLabel;
class QListWidget;
class QTimer;
QT_END_NAMESPACE

class ReplayDock : public QWidget {
	Q_OBJECT
public:
	explicit ReplayDock(QWidget *parent = nullptr);
	~ReplayDock() override;

private slots:
	void onToggleClicked();
	void onReturnClicked();
	void onAngleClicked(int index);
	void onSceneItemChanged();
	void reloadScenes();
	void refresh();
	void ensureControlsButton(); // inject a button into OBS's Controls dock

private:
	std::vector<std::string> checkedScenes() const;
	void loadSelection();
	void saveSelection();
	void pushSelectionToController();
	void installStatusBarIndicator();
	void removeStatusBarIndicator();
	void removeControlsButton();

	QPushButton *toggleBtn_ = nullptr;
	QLabel *statusLabel_ = nullptr;
	QListWidget *sceneList_ = nullptr;
	QPushButton *returnBtn_ = nullptr;
	QPushButton *angleBtns_[4] = {};
	QTimer *timer_ = nullptr;
	QLabel *statusBarLabel_ = nullptr;
	QPushButton *controlsBtn_ = nullptr; // lives inside OBS's Controls dock

	std::vector<std::string> selection_; // persisted scene selection
	bool updatingList_ = false;          // guard against re-entrant item signals
};

// Called from obs_module_load / obs_module_unload.
void register_replay_dock();
void unregister_replay_dock();
