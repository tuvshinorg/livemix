/******************************************************************************
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/* Wirecast's menu bar: File, Edit, Switch, Media, Output, Layout, Replay + ISO,
 * Window, Help.
 *
 * The menus are rebuilt from OBS's own actions and sub-menus wherever OBS has
 * the feature, so plugins that add to Tools, Docks, Profile or Scene
 * Collection keep working. A Wirecast "document" is an OBS scene collection. */

#include "OBSBasic.hpp"

#include <components/BroadcastBar.hpp>
#include <components/LayersPanel.hpp>
#include <components/TransitionBar.hpp>
#include <components/WirecastAudioMeter.hpp>
#include <components/WirecastSidebar.hpp>

#include <qt-wrappers.hpp>

#include <QActionGroup>
#include <QApplication>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cstring>
#include <random>

namespace {
constexpr const char *kWirecastSection = "Wirecast";

/* OBSBasicSettings page order */
constexpr int kSettingsOutputPage = 3;
constexpr int kSettingsVideoPage = 5;

/* Wirecast's transition speeds, slowest to fastest (ms) */
constexpr int kTransitionSpeeds[] = {2000, 1000, 500, 250, 100};
constexpr const char *kTransitionSpeedNames[] = {"Slowest", "Slow", "Normal", "Fast", "Fastest"};

/* Wirecast's Go to Layer keys: Title, Foreground, Normal, Background, Audio */
constexpr Qt::Key kGoToLayerKeys[] = {Qt::Key_T, Qt::Key_F, Qt::Key_N, Qt::Key_B, Qt::Key_A};

struct CanvasPreset {
	uint32_t cx;
	uint32_t cy;
};
constexpr CanvasPreset kCanvasPresets[] = {{640, 360},   {854, 480},   {1280, 720},  {1920, 1080},
					   {2560, 1440}, {3840, 2160}, {720, 1280},  {1080, 1920}};

QAction *addShortcutAction(QMenu *menu, const QString &text, const QKeySequence &shortcut)
{
	QAction *action = menu->addAction(text);
	action->setShortcut(shortcut);
	return action;
}

std::vector<OBSSource> mediaSources()
{
	std::vector<OBSSource> result;
	auto collect = [](void *param, obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		}
		return true;
	};
	obs_enum_sources(collect, &result);
	return result;
}

/* Sources directly inside a shot, or every source when no shot is selected */
std::vector<OBSSource> playlistSources(obs_source_t *shot)
{
	std::vector<OBSSource> result;
	auto collect = [](void *param, obs_source_t *source) {
		static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		return true;
	};

	if (obs_scene_t *scene = shot ? obs_scene_from_source(shot) : nullptr) {
		auto addItem = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(obs_sceneitem_get_source(item));
			return true;
		};
		obs_scene_enum_items(scene, addItem, &result);
	} else {
		obs_enum_sources(collect, &result);
	}
	return result;
}

/* Cut / Copy / Paste / Select All act on the text field that has focus */
QWidget *focusedTextWidget()
{
	QWidget *focus = QApplication::focusWidget();
	if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) ||
	    qobject_cast<QPlainTextEdit *>(focus)) {
		return focus;
	}
	return nullptr;
}

bool textCommand(const char *method)
{
	QWidget *widget = focusedTextWidget();
	if (!widget) {
		return false;
	}
	QMetaObject::invokeMethod(widget, method);
	return true;
}
} // namespace

void OBSBasic::OpenSettingsPage(int page)
{
	pendingSettingsPage = page;
	on_action_Settings_triggered();
	pendingSettingsPage = -1;
}

bool OBSBasic::WirecastFinalMixVisible() const
{
	config_t *config = App()->GetUserConfig();
	return !config_has_user_value(config, kWirecastSection, "FinalMix") ||
	       config_get_bool(config, kWirecastSection, "FinalMix");
}

int OBSBasic::WirecastMonitorMode() const
{
	if (!IsPreviewProgramMode()) {
		return BroadcastBar::LiveOnly;
	}
	return config_get_bool(App()->GetUserConfig(), kWirecastSection, "PreviewOnly") ? BroadcastBar::PreviewOnly
											: BroadcastBar::LiveAndPreview;
}

void OBSBasic::SetWirecastMonitorMode(int mode)
{
	if (mode == BroadcastBar::LiveOnly) {
		if (IsPreviewProgramMode()) {
			SetPreviewProgramMode(false);
		}
		ApplyWirecastMonitorMode();
		return;
	}

	config_set_bool(App()->GetUserConfig(), kWirecastSection, "PreviewOnly", mode == BroadcastBar::PreviewOnly);
	if (!IsPreviewProgramMode()) {
		SetPreviewProgramMode(true);
	}
	ApplyWirecastMonitorMode();
}

void OBSBasic::ApplyWirecastMonitorMode()
{
	const int mode = WirecastMonitorMode();
	const bool finalMix = WirecastFinalMixVisible();

	if (programWidget) {
		programWidget->setVisible(mode != BroadcastBar::PreviewOnly);
	}
	if (monitorGrip) {
		monitorGrip->setVisible(mode != BroadcastBar::PreviewOnly);
	}
	if (previewMeter) {
		previewMeter->setVisible(finalMix && mode != BroadcastBar::LiveOnly);
	}
	if (programMeter) {
		programMeter->setVisible(finalMix && mode != BroadcastBar::PreviewOnly);
	}
	if (broadcastBar) {
		broadcastBar->setMonitorMode(mode);
	}
}

/* OBS ignores a start request while Replay Buffer is off in Output settings; say so instead */
void OBSBasic::WirecastReplayToggle()
{
	if (outputHandler && outputHandler->replayBuffer) {
		ReplayBufferActionTriggered();
		return;
	}

	if (OBSMessageBox::question(this, QTStr("Wirecast.Replay.Off.Title"), QTStr("Wirecast.Replay.Off.Text")) ==
	    QMessageBox::Yes) {
		OpenSettingsPage(kSettingsOutputPage);
	}
}

void OBSBasic::WirecastMenuEvent(enum obs_frontend_event event, void *data)
{
	OBSBasic *main = static_cast<OBSBasic *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		/* Replay + ISO > Create / Update Replay Shot */
		if (main->pendingReplayShot != ReplayShotRequest::None && main->layersPanel) {
			const bool update = main->pendingReplayShot == ReplayShotRequest::Update;
			main->pendingReplayShot = ReplayShotRequest::None;
			char *path = obs_frontend_get_last_replay();
			main->layersPanel->createReplayShot(QT_UTF8(path), update);
			bfree(path);
		}
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		main->ApplyWirecastMonitorMode();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_event_callback(&OBSBasic::WirecastMenuEvent, main);
		break;
	default:
		break;
	}
}

void OBSBasic::SetupWirecastMenus()
{
	QMenuBar *bar = ui->menubar;
	bar->clear();

	/* Wirecast's shot shortcuts win over OBS's transform shortcuts, which move aside */
	ui->actionEditTransform->setShortcut(QKeySequence("Ctrl+Shift+E"));
	ui->actionResetTransform->setShortcut(QKeySequence("Ctrl+Shift+R"));
	ui->actionCenterToScreen->setShortcut(QKeySequence("Ctrl+Shift+D"));
	ui->actionStretchToScreen->setShortcut(QKeySequence());
	ui->actionCopySource->setShortcut(QKeySequence());
	ui->actionPasteRef->setShortcut(QKeySequence());

	auto selectedShot = []() { return LayersPanel::selectedShot(); };

	/* ================================================================ */
	/* File                                                             */

	QMenu *fileMenu = bar->addMenu(QTStr("Basic.MainMenu.File"));

	QAction *newAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.New"), QKeySequence::New);
	connect(newAction, &QAction::triggered, this, &OBSBasic::on_actionNewSceneCollection_triggered);

	QAction *openAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.Open"), QKeySequence::Open);
	connect(openAction, &QAction::triggered, this, &OBSBasic::on_actionImportSceneCollection_triggered);

	QMenu *recentMenu = fileMenu->addMenu(QTStr("Wirecast.File.OpenRecent"));
	connect(recentMenu, &QMenu::aboutToShow, this, [recentMenu]() {
		recentMenu->clear();
		char **names = obs_frontend_get_scene_collections();
		char *current = obs_frontend_get_current_scene_collection();
		for (char **name = names; name && *name; name++) {
			const QString collection = QT_UTF8(*name);
			QAction *action = recentMenu->addAction(collection, recentMenu, [collection]() {
				obs_frontend_set_current_scene_collection(QT_TO_UTF8(collection));
			});
			action->setCheckable(true);
			action->setChecked(current && collection == QT_UTF8(current));
		}
		bfree(current);
		bfree(names);
	});

	/* The documents window; it no longer opens by itself at startup */
	QAction *documentsAction =
		addShortcutAction(fileMenu, QTStr("Wirecast.Documents.Menu"), QKeySequence("Ctrl+Shift+O"));
	connect(documentsAction, &QAction::triggered, this, &OBSBasic::ShowWelcome);

	QAction *importAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.ImportMedia"), QKeySequence("Ctrl+I"));
	connect(importAction, &QAction::triggered, this, [this]() {
		const QStringList files = QFileDialog::getOpenFileNames(this, QTStr("Wirecast.File.ImportMedia"));
		if (layersPanel && !files.isEmpty()) {
			layersPanel->importMedia(files);
		}
	});

	fileMenu->addSeparator();
	QAction *saveAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.Save"), QKeySequence::Save);
	connect(saveAction, &QAction::triggered, this, &OBSBasic::SaveProjectNow);
	QAction *saveAsAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.SaveAs"), QKeySequence::SaveAs);
	connect(saveAsAction, &QAction::triggered, this, &OBSBasic::on_actionDupSceneCollection_triggered);
	QAction *closeAction = addShortcutAction(fileMenu, QTStr("Wirecast.File.CloseWindow"), QKeySequence::Close);
	connect(closeAction, &QAction::triggered, this, [this]() {
		QWidget *window = QApplication::activeWindow();
		(window ? window : this)->close();
	});

	fileMenu->addSeparator();
	fileMenu->addMenu(ui->sceneCollectionMenu);
	fileMenu->addMenu(ui->profileMenu);

	fileMenu->addSeparator();
	QAction *preferences = fileMenu->addAction(QTStr("Wirecast.File.Preferences"));
	preferences->setShortcut(QKeySequence::Preferences);
	connect(preferences, &QAction::triggered, this, &OBSBasic::on_action_Settings_triggered);

	fileMenu->addSeparator();
	fileMenu->addAction(ui->actionAlwaysOnTop);
	fileMenu->addAction(ui->actionE_xit);

	/* ================================================================ */
	/* Edit                                                             */

	QMenu *editMenu = bar->addMenu(QTStr("Basic.MainMenu.Edit"));
	editMenu->addAction(ui->actionMainUndo);
	editMenu->addAction(ui->actionMainRedo);
	editMenu->addSeparator();

	QAction *cutAction = addShortcutAction(editMenu, QTStr("Wirecast.Edit.Cut"), QKeySequence::Cut);
	connect(cutAction, &QAction::triggered, this, []() { textCommand("cut"); });

	QAction *copyAction = addShortcutAction(editMenu, QTStr("Wirecast.Edit.Copy"), QKeySequence::Copy);
	connect(copyAction, &QAction::triggered, this, [this]() {
		if (!textCommand("copy")) {
			ui->actionCopySource->trigger();
		}
	});

	QAction *pasteAction = addShortcutAction(editMenu, QTStr("Wirecast.Edit.Paste"), QKeySequence::Paste);
	connect(pasteAction, &QAction::triggered, this, [this]() {
		if (!textCommand("paste")) {
			ui->actionPasteRef->trigger();
		}
	});

	QAction *consolidated = editMenu->addAction(QTStr("Wirecast.Edit.PasteConsolidated"));
	connect(consolidated, &QAction::triggered, this, [this]() {
		if (!layersPanel) {
			return;
		}

		std::vector<OBSSource> sources;
		std::vector<const SourceCopyInfo *> infos;
		for (const SourceCopyInfo &info : clipboard) {
			OBSSource source = OBSGetStrongRef(info.weak_source);
			if (source && !obs_source_removed(source)) {
				sources.push_back(source);
				infos.push_back(&info);
			}
		}
		if (sources.empty()) {
			return;
		}

		layersPanel->createShot(QTStr("Wirecast.Edit.ConsolidatedShot"), sources,
					[&infos](obs_sceneitem_t *item, size_t index) {
						const SourceCopyInfo *info = infos[index];
						obs_sceneitem_set_info2(item, &info->transform);
						obs_sceneitem_set_crop(item, &info->crop);
						obs_sceneitem_set_visible(item, info->visible);
					});
	});

	QAction *selectAll = addShortcutAction(editMenu, QTStr("Wirecast.Edit.SelectAll"), QKeySequence::SelectAll);
	connect(selectAll, &QAction::triggered, this, []() { textCommand("selectAll"); });

	editMenu->addSeparator();
	QAction *editShot = addShortcutAction(editMenu, QTStr("Basic.Layers.EditShot"), QKeySequence("Ctrl+E"));
	connect(editShot, &QAction::triggered, this, [this, selectedShot]() {
		if (OBSSource shot = selectedShot(); shot && layersPanel) {
			layersPanel->editShot(shot);
		}
	});
	QAction *renameShot = addShortcutAction(editMenu, QTStr("Wirecast.Shot.Rename"), QKeySequence("Ctrl+R"));
	connect(renameShot, &QAction::triggered, this, [this, selectedShot]() {
		if (OBSSource shot = selectedShot(); shot && layersPanel) {
			layersPanel->renameShot(shot);
		}
	});
	QAction *duplicateShot =
		addShortcutAction(editMenu, QTStr("Wirecast.Shot.Duplicate"), QKeySequence("Ctrl+D"));
	connect(duplicateShot, &QAction::triggered, this, [this, selectedShot]() {
		if (OBSSource shot = selectedShot(); shot && layersPanel) {
			layersPanel->duplicateShot(shot);
		}
	});
	QMenu *dupOptionsHost = editMenu->addMenu(QTStr("Wirecast.Shot.DuplicateOptions"));
	QAction *deleteShot =
		addShortcutAction(editMenu, QTStr("Wirecast.Shot.Delete"), QKeySequence("Ctrl+Backspace"));
	connect(deleteShot, &QAction::triggered, this, [this, selectedShot]() {
		if (OBSSource shot = selectedShot(); shot && layersPanel) {
			layersPanel->deleteShot(shot);
		}
	});
	QMenu *moveHost = editMenu->addMenu(QTStr("Basic.Layers.MoveToLayer"));

	editMenu->addSeparator();
	editMenu->addAction(ui->actionPasteDup);
	editMenu->addAction(ui->actionCopyFilters);
	editMenu->addAction(ui->actionPasteFilters);
	editMenu->addMenu(ui->transformMenu);
	editMenu->addMenu(ui->orderMenu);
	editMenu->addMenu(ui->scalingMenu);
	editMenu->addAction(ui->actionLockPreview);

	connect(editMenu, &QMenu::aboutToShow, this, [this, selectedShot, cutAction, selectAll, editShot, renameShot,
						      duplicateShot, deleteShot, dupOptionsHost, moveHost]() {
		const bool text = focusedTextWidget() != nullptr;
		cutAction->setEnabled(text);
		selectAll->setEnabled(text);

		/* Rebuild the shot sub-menus for the shot selected right now */
		OBSSource shot = selectedShot();
		for (QAction *action : {editShot, renameShot, duplicateShot, deleteShot}) {
			action->setEnabled(shot != nullptr);
		}

		dupOptionsHost->clear();
		moveHost->clear();
		if (shot && layersPanel) {
			layersPanel->fillDuplicateOptionsMenu(dupOptionsHost, shot);
			layersPanel->fillMoveToLayerMenu(moveHost, shot);
		}
		dupOptionsHost->setEnabled(!dupOptionsHost->isEmpty());
		moveHost->setEnabled(!moveHost->isEmpty());
	});

	/* ================================================================ */
	/* Switch                                                           */

	QMenu *switchMenu = bar->addMenu(QTStr("Wirecast.Menu.Switch"));

	QAction *goAction = addShortcutAction(switchMenu, QTStr("Wirecast.Switch.Go"), QKeySequence("Ctrl+G"));
	connect(goAction, &QAction::triggered, this, []() {
		if (obs_frontend_preview_program_mode_active() && !LayersPanel::autoLive()) {
			obs_frontend_preview_program_trigger_transition();
		}
	});

	switchMenu->addSeparator();
	QActionGroup *slotGroup = new QActionGroup(switchMenu);
	std::vector<QAction *> slotActions;
	for (int slot = 0; slot < 2; slot++) {
		QAction *action = addShortcutAction(switchMenu, QString(), QKeySequence(QString("Ctrl+%1").arg(slot + 1)));
		action->setCheckable(true);
		slotGroup->addAction(action);
		slotActions.push_back(action);
		connect(action, &QAction::triggered, this, [this, slot]() {
			if (transitionBar) {
				transitionBar->selectSlot(slot);
			}
		});
	}

	QMenu *speedMenu = switchMenu->addMenu(QTStr("Wirecast.Switch.TransitionSpeed"));
	QActionGroup *speedGroup = new QActionGroup(speedMenu);
	for (size_t i = 0; i < std::size(kTransitionSpeeds); i++) {
		QAction *action =
			speedMenu->addAction(QTStr((std::string("Wirecast.Switch.Speed.") + kTransitionSpeedNames[i]).c_str()));
		action->setCheckable(true);
		action->setData(kTransitionSpeeds[i]);
		speedGroup->addAction(action);
		const int duration = kTransitionSpeeds[i];
		connect(action, &QAction::triggered, this, [this, duration]() { SetTransitionDuration(duration); });
	}

	QAction *manageStingers = switchMenu->addAction(QTStr("Wirecast.Switch.ManageStingers"));
	connect(manageStingers, &QAction::triggered, this, [this]() { ShowWirecastDock(ui->transitionsDock); });
	QAction *addStinger = switchMenu->addAction(QTStr("Wirecast.Switch.AddStinger"));
	connect(addStinger, &QAction::triggered, this, [this]() { AddTransition("obs_stinger_transition"); });

	switchMenu->addSeparator();
	QAction *autoLiveAction = switchMenu->addAction(QTStr("Wirecast.Switch.AutoLive"));
	autoLiveAction->setCheckable(true);
	connect(autoLiveAction, &QAction::toggled, this, [this](bool enabled) {
		LayersPanel::setAutoLive(enabled);
		if (transitionBar) {
			transitionBar->setAutoLive(enabled);
		}
	});
	autoLiveAction->setChecked(LayersPanel::autoLive());
	if (transitionBar) {
		transitionBar->setAutoLive(LayersPanel::autoLive());
		/* The Auto live checkbox under the monitors drives the same setting */
		connect(transitionBar, &TransitionBar::autoLiveToggled, autoLiveAction, &QAction::setChecked);
	}

	QAction *swapAction = switchMenu->addAction(QTStr("Wirecast.Switch.LivePreviewSwap"));
	swapAction->setCheckable(true);
	connect(swapAction, &QAction::toggled, this, [this](bool enabled) {
		swapScenesMode = enabled;
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SwapScenesMode", enabled);
	});

	QMenu *visibilityMenu = switchMenu->addMenu(QTStr("Wirecast.Switch.LayerVisibility"));
	std::vector<QAction *> visibilityActions;
	for (int layer = 1; layer <= LayersPanel::kLayerCount; layer++) {
		QAction *action = addShortcutAction(visibilityMenu, QTStr("Basic.Layers.Layer").arg(layer),
						    QKeySequence(QString("Alt+%1").arg(layer)));
		action->setCheckable(true);
		visibilityActions.push_back(action);
		/* Alt+N works with the menu closed too, so toggle from the real state, not the check mark */
		connect(action, &QAction::triggered, this, [this, layer]() {
			if (layersPanel) {
				layersPanel->setLayerVisible(layer, !LayersPanel::isLayerVisible(layer));
			}
		});
	}

	connect(switchMenu, &QMenu::aboutToShow, this,
		[this, goAction, slotActions, speedGroup, autoLiveAction, swapAction, visibilityActions]() {
			goAction->setEnabled(IsPreviewProgramMode() && !LayersPanel::autoLive());
			for (int slot = 0; slot < int(slotActions.size()); slot++) {
				slotActions[slot]->setText(transitionBar ? transitionBar->slotText(slot) : QString());
				slotActions[slot]->setChecked(transitionBar && transitionBar->currentSlot() == slot);
			}
			const int duration = GetTransitionDuration();
			for (QAction *action : speedGroup->actions()) {
				action->setChecked(action->data().toInt() == duration);
			}
			QSignalBlocker blockAuto(autoLiveAction);
			autoLiveAction->setChecked(LayersPanel::autoLive());
			QSignalBlocker blockSwap(swapAction);
			swapAction->setChecked(swapScenesMode);
			for (int layer = 1; layer <= int(visibilityActions.size()); layer++) {
				visibilityActions[layer - 1]->setChecked(LayersPanel::isLayerVisible(layer));
			}
		});

	/* ================================================================ */
	/* Media                                                            */

	QMenu *mediaMenu = bar->addMenu(QTStr("Wirecast.Menu.Media"));

	QAction *playAll = mediaMenu->addAction(QTStr("Wirecast.Media.PlayAll"));
	connect(playAll, &QAction::triggered, this, []() {
		for (const OBSSource &source : mediaSources()) {
			const obs_media_state state = obs_source_media_get_state(source);
			if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED) {
				obs_source_media_restart(source);
			} else if (state != OBS_MEDIA_STATE_PLAYING) {
				obs_source_media_play_pause(source, false);
			}
		}
	});

	QAction *pauseAll = mediaMenu->addAction(QTStr("Wirecast.Media.PauseAll"));
	connect(pauseAll, &QAction::triggered, this, []() {
		for (const OBSSource &source : mediaSources()) {
			if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING) {
				obs_source_media_play_pause(source, true);
			}
		}
	});

	QAction *shuffle = mediaMenu->addAction(QTStr("Wirecast.Media.Shuffle"));
	shuffle->setToolTip(QTStr("Wirecast.Media.Shuffle.Tooltip"));
	connect(shuffle, &QAction::triggered, this, [selectedShot]() {
		static std::mt19937 random{std::random_device{}()};

		for (const OBSSource &source : playlistSources(selectedShot())) {
			const char *id = obs_source_get_unversioned_id(source);
			OBSDataAutoRelease settings = obs_source_get_settings(source);

			if (strcmp(id, "vlc_source") == 0) {
				/* Reorder the playlist itself, then restart it from the new first item */
				OBSDataArrayAutoRelease playlist = obs_data_get_array(settings, "playlist");
				const size_t count = obs_data_array_count(playlist);
				if (count < 2) {
					continue;
				}
				std::vector<OBSDataAutoRelease> entries;
				for (size_t i = 0; i < count; i++) {
					entries.emplace_back(obs_data_array_item(playlist, i));
				}
				std::shuffle(entries.begin(), entries.end(), random);
				OBSDataArrayAutoRelease shuffled = obs_data_array_create();
				for (const OBSDataAutoRelease &entry : entries) {
					obs_data_array_push_back(shuffled, entry);
				}
				obs_data_set_array(settings, "playlist", shuffled);
				obs_source_update(source, settings);
				obs_source_media_restart(source);
			} else if (strcmp(id, "slideshow") == 0) {
				obs_data_set_bool(settings, "randomize", true);
				obs_source_update(source, settings);
			}
		}
	});

	mediaMenu->addSeparator();
	QAction *assetManager = mediaMenu->addAction(QTStr("Wirecast.Media.AssetManager"));
	connect(assetManager, &QAction::triggered, this, &OBSBasic::on_actionShowMissingFiles_triggered);

	/* ================================================================ */
	/* Output                                                           */

	QMenu *outputMenu = bar->addMenu(QTStr("Wirecast.Menu.Output"));

	QAction *outputSettings = outputMenu->addAction(QTStr("Wirecast.Output.Settings"));
	connect(outputSettings, &QAction::triggered, this, [this]() { OpenSettingsPage(kSettingsOutputPage); });

	outputMenu->addSeparator();
	QAction *streamAction = outputMenu->addAction(QString());
	connect(streamAction, &QAction::triggered, this, &OBSBasic::StreamActionTriggered);
	QAction *recordAction = outputMenu->addAction(QString());
	connect(recordAction, &QAction::triggered, this, &OBSBasic::RecordActionTriggered);
	outputMenu->addAction(ui->actionShow_Recordings);
	outputMenu->addAction(ui->actionRemux);

	outputMenu->addSeparator();
	ui->multiviewProjectorMenu->setTitle(QTStr("Wirecast.Output.MultiViewer"));
	outputMenu->addMenu(ui->multiviewProjectorMenu);
	connect(ui->multiviewProjectorMenu, &QMenu::aboutToShow, this, &OBSBasic::updateMultiviewProjectorMenu);

	QMenu *secondaryMenu = outputMenu->addMenu(QTStr("Wirecast.Output.SecondaryDisplay"));
	connect(secondaryMenu, &QMenu::aboutToShow, this, [this, secondaryMenu]() {
		secondaryMenu->clear();
		AddProjectorMenuMonitors(secondaryMenu, this, &OBSBasic::OpenStudioProgramProjector);
		secondaryMenu->addSeparator();
		secondaryMenu->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenStudioProgramWindow);
	});

	QAction *virtualCam = outputMenu->addAction(QTStr("Wirecast.Output.VirtualCamera"));
	virtualCam->setCheckable(true);
	connect(virtualCam, &QAction::triggered, this, [this, virtualCam]() {
		virtualCam->setChecked(obs_frontend_virtualcam_active());
		VirtualCamActionTriggered();
	});

	outputMenu->addSeparator();
	QMenu *canvasMenu = outputMenu->addMenu(QTStr("Wirecast.Output.CanvasSize"));
	connect(canvasMenu, &QMenu::aboutToShow, this, [this, canvasMenu]() {
		canvasMenu->clear();
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		const bool active = Active();

		for (const CanvasPreset &preset : kCanvasPresets) {
			QAction *action = canvasMenu->addAction(QString("%1 x %2").arg(preset.cx).arg(preset.cy));
			action->setCheckable(true);
			action->setChecked(ovi.base_width == preset.cx && ovi.base_height == preset.cy);
			action->setEnabled(!active);
			const CanvasPreset size = preset;
			connect(action, &QAction::triggered, this, [this, size]() {
				/* The output (scaled) resolution set in Settings stays, unless the new canvas has a
				 * different shape, which would stretch the picture; then the output follows the canvas */
				const uint64_t outCX = config_get_uint(activeConfiguration, "Video", "OutputCX");
				const uint64_t outCY = config_get_uint(activeConfiguration, "Video", "OutputCY");
				if (outCX * size.cy != outCY * size.cx) {
					config_set_uint(activeConfiguration, "Video", "OutputCX", size.cx);
					config_set_uint(activeConfiguration, "Video", "OutputCY", size.cy);
				}
				config_set_uint(activeConfiguration, "Video", "BaseCX", size.cx);
				config_set_uint(activeConfiguration, "Video", "BaseCY", size.cy);
				config_save_safe(activeConfiguration, "tmp", nullptr);
				ResetVideo();
			});
		}
		canvasMenu->addSeparator();
		QAction *custom = canvasMenu->addAction(QTStr("Wirecast.Output.CanvasCustom"));
		connect(custom, &QAction::triggered, this, [this]() { OpenSettingsPage(kSettingsVideoPage); });
	});

	QAction *statistics = outputMenu->addAction(QTStr("Wirecast.Output.Statistics"));
	connect(statistics, &QAction::triggered, this, &OBSBasic::on_stats_triggered);

	connect(outputMenu, &QMenu::aboutToShow, this, [streamAction, recordAction, virtualCam]() {
		streamAction->setText(obs_frontend_streaming_active() ? QTStr("Wirecast.Output.StopBroadcasting")
								      : QTStr("Wirecast.Output.StartBroadcasting"));
		recordAction->setText(obs_frontend_recording_active() ? QTStr("Wirecast.Output.StopRecording")
								      : QTStr("Wirecast.Output.StartRecording"));
		virtualCam->setChecked(obs_frontend_virtualcam_active());
	});

	/* ================================================================ */
	/* Layout                                                           */

	QMenu *layoutMenu = bar->addMenu(QTStr("Wirecast.Menu.Layout"));

	QAction *finalMix = layoutMenu->addAction(QTStr("Wirecast.Layout.FinalMix"));
	finalMix->setCheckable(true);
	connect(finalMix, &QAction::triggered, this, [this](bool checked) {
		config_set_bool(App()->GetUserConfig(), kWirecastSection, "FinalMix", checked);
		ApplyWirecastMonitorMode();
	});

	layoutMenu->addSeparator();
	QActionGroup *modeGroup = new QActionGroup(layoutMenu);
	const std::pair<int, const char *> modes[] = {{BroadcastBar::LiveAndPreview, "Wirecast.Layout.ShowBoth"},
						      {BroadcastBar::PreviewOnly, "Wirecast.Layout.ShowPreview"},
						      {BroadcastBar::LiveOnly, "Wirecast.Layout.ShowLive"}};
	for (const auto &[mode, text] : modes) {
		QAction *action = layoutMenu->addAction(QTStr(text));
		action->setCheckable(true);
		action->setData(mode);
		modeGroup->addAction(action);
		const int target = mode;
		connect(action, &QAction::triggered, this, [this, target]() { SetWirecastMonitorMode(target); });
	}

	layoutMenu->addSeparator();
	QAction *zoomIn = addShortcutAction(layoutMenu, QTStr("Wirecast.Layout.ZoomIn"), QKeySequence("Ctrl+="));
	connect(zoomIn, &QAction::triggered, this, [this]() {
		if (layersPanel) {
			layersPanel->zoomAllLayers(25);
		}
	});
	QAction *zoomOut = addShortcutAction(layoutMenu, QTStr("Wirecast.Layout.ZoomOut"), QKeySequence("Ctrl+-"));
	connect(zoomOut, &QAction::triggered, this, [this]() {
		if (layersPanel) {
			layersPanel->zoomAllLayers(-25);
		}
	});
	QMenu *zoomTo = layoutMenu->addMenu(QTStr("Wirecast.Layout.ZoomTo"));
	for (int percent : {50, 100, 200}) {
		zoomTo->addAction(QString("%1%").arg(percent), this, [this, percent]() {
			if (layersPanel) {
				layersPanel->setAllLayersZoom(percent);
			}
		});
	}

	QMenu *goToLayer = layoutMenu->addMenu(QTStr("Wirecast.Layout.GoToLayer"));
	for (int layer = 1; layer <= LayersPanel::kLayerCount; layer++) {
		QAction *action =
			addShortcutAction(goToLayer, QTStr("Basic.Layers.Layer").arg(layer),
					  QKeySequence(Qt::CTRL | Qt::SHIFT | kGoToLayerKeys[layer - 1]));
		connect(action, &QAction::triggered, this, [this, layer]() {
			if (layersPanel) {
				layersDock->show();
				layersPanel->goToLayer(layer);
			}
		});
	}

	layoutMenu->addSeparator();
	layoutMenu->addMenu(ui->menuDocks);
	layoutMenu->addAction(ui->toggleStatusBar);
	if (ui->actionFullscreenInterface) {
		layoutMenu->addAction(ui->actionFullscreenInterface);
	}
	layoutMenu->addAction(ui->resetUI);

	connect(layoutMenu, &QMenu::aboutToShow, this, [this, finalMix, modeGroup]() {
		finalMix->setChecked(WirecastFinalMixVisible());
		const int mode = WirecastMonitorMode();
		for (QAction *action : modeGroup->actions()) {
			action->setChecked(action->data().toInt() == mode);
		}
	});

	/* ================================================================ */
	/* Replay + ISO                                                     */

	QMenu *replayMenu = bar->addMenu(QTStr("Wirecast.Menu.Replay"));

	QAction *replayStartAll = replayMenu->addAction(QString());
	connect(replayStartAll, &QAction::triggered, this, &OBSBasic::WirecastReplayToggle);

	QMenu *programReplay = replayMenu->addMenu(QTStr("Wirecast.Replay.Program"));
	QAction *programStart = programReplay->addAction(QString());
	connect(programStart, &QAction::triggered, this, &OBSBasic::WirecastReplayToggle);

	auto requestReplayShot = [this](ReplayShotRequest request) {
		if (!obs_frontend_replay_buffer_active()) {
			return;
		}
		pendingReplayShot = request;
		ReplayBufferSave();
	};
	QAction *createReplay = programReplay->addAction(QTStr("Wirecast.Replay.Create"));
	connect(createReplay, &QAction::triggered, this,
		[requestReplayShot]() { requestReplayShot(ReplayShotRequest::Create); });
	QAction *updateReplay = programReplay->addAction(QTStr("Wirecast.Replay.Update"));
	connect(updateReplay, &QAction::triggered, this,
		[requestReplayShot]() { requestReplayShot(ReplayShotRequest::Update); });

	replayMenu->addSeparator();
	QAction *createAll = replayMenu->addAction(QTStr("Wirecast.Replay.CreateAll"));
	connect(createAll, &QAction::triggered, this,
		[requestReplayShot]() { requestReplayShot(ReplayShotRequest::Create); });
	QAction *updateAll = replayMenu->addAction(QTStr("Wirecast.Replay.UpdateAll"));
	connect(updateAll, &QAction::triggered, this,
		[requestReplayShot]() { requestReplayShot(ReplayShotRequest::Update); });

	replayMenu->addSeparator();
	QAction *replayConfigure = replayMenu->addAction(QTStr("Wirecast.Replay.Configure"));
	connect(replayConfigure, &QAction::triggered, this, [this]() { OpenSettingsPage(kSettingsOutputPage); });

	connect(replayMenu, &QMenu::aboutToShow, this,
		[replayStartAll, programStart, createReplay, updateReplay, createAll, updateAll]() {
			const bool active = obs_frontend_replay_buffer_active();
			replayStartAll->setText(active ? QTStr("Wirecast.Replay.StopAll")
						       : QTStr("Wirecast.Replay.StartAll"));
			programStart->setText(active ? QTStr("Wirecast.Replay.Stop") : QTStr("Wirecast.Replay.Start"));
			for (QAction *action : {createReplay, updateReplay, createAll, updateAll}) {
				action->setEnabled(active);
			}
		});

	/* ================================================================ */
	/* Window                                                           */

	QMenu *windowMenu = bar->addMenu(QTStr("Wirecast.Menu.Window"));

	QAction *encoderPresets = windowMenu->addAction(QTStr("Wirecast.Window.EncoderPresets"));
	connect(encoderPresets, &QAction::triggered, this, [this]() { OpenSettingsPage(kSettingsOutputPage); });

	QAction *audioMixer = windowMenu->addAction(QTStr("Wirecast.Window.AudioMixer"));
	connect(audioMixer, &QAction::triggered, this, [this]() { ShowWirecastDock(ui->mixerDock); });
	windowMenu->addAction(ui->actionAdvAudioProperties);
	QAction *notifications = windowMenu->addAction(QTStr("Wirecast.Window.Notifications"));
	connect(notifications, &QAction::triggered, ui->actionViewCurrentLog, &QAction::trigger);

	windowMenu->addSeparator();
	QMenu *layerWindows = windowMenu->addMenu(QTStr("Wirecast.Window.NewLayerWindow"));
	for (int layer = 1; layer <= LayersPanel::kLayerCount; layer++) {
		layerWindows->addAction(QTStr("Basic.Layers.Layer").arg(layer), this, [this, layer]() {
			QWidget *window = new QWidget(this, Qt::Tool);
			window->setAttribute(Qt::WA_DeleteOnClose);
			window->setWindowTitle(QTStr("Basic.Layers.Layer").arg(layer));
			QVBoxLayout *windowLayout = new QVBoxLayout(window);
			windowLayout->setContentsMargins(0, 0, 0, 0);
			windowLayout->addWidget(new LayersPanel(window, layer));
			window->resize(720, 130);
			window->show();
		});
	}

	windowMenu->addSeparator();
	ui->menuTools->setTitle(QTStr("Basic.MainMenu.Tools"));
	windowMenu->addMenu(ui->menuTools);

	/* ================================================================ */
	/* Help                                                             */

	QMenu *helpMenu = ui->menuBasic_MainMenu_Help;
	helpMenu->addAction(ui->actionShowSettingsFolder);
	helpMenu->addAction(ui->actionShowProfileFolder);
	bar->addMenu(helpMenu);

	/* ================================================================ */
	/* Number keys take the shots mapped with Keyboard Shortcut            */

	for (int key = 0; key <= 9; key++) {
		const int shotKey = key == 0 ? 10 : key;
		const Qt::Key digit = Qt::Key(Qt::Key_0 + key);
		for (const QKeySequence &sequence : {QKeySequence(QKeyCombination(digit)),
						     QKeySequence(QKeyCombination(Qt::KeypadModifier, digit))}) {
			QShortcut *shortcut = new QShortcut(sequence, this);
			shortcut->setContext(Qt::WindowShortcut);
			connect(shortcut, &QShortcut::activated, this, [this, shotKey]() {
				if (layersPanel) {
					layersPanel->takeShotsWithHotkey(shotKey);
				}
			});
		}
	}

	obs_frontend_add_event_callback(&OBSBasic::WirecastMenuEvent, this);
}
