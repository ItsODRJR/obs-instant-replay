/*
OBS Instant Replay
Copyright (C) 2026 itsodrjr <itsodrjr@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "replay-output.hpp"
#include "replay-source.hpp"
#include "replay-controller.hpp"
#ifdef HAVE_QT_DOCK
#include "replay-dock.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// The video/audio pipeline and scenes are ready only now, so this
		// is the safe point to create the encoder + output and start
		// filling the ring buffer.
		if (!ReplayController::instance().start())
			obs_log(LOG_ERROR, "instant replay failed to start");
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		ReplayController::instance().shutdown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	// Register the types up front; the controller instantiates them later.
	register_replay_output();
	register_replay_source();
	register_replay_hotkeys();

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

#ifdef HAVE_QT_DOCK
	// The dock must be created on the UI thread; obs_module_load runs there.
	register_replay_dock();
#endif

	obs_log(LOG_INFO, "instant replay loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
#ifdef HAVE_QT_DOCK
	unregister_replay_dock();
#endif
	unregister_replay_hotkeys();
	ReplayController::instance().shutdown();
	obs_log(LOG_INFO, "instant replay unloaded");
}
