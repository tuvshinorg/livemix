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

/* Documents: the documents window (File > Documents), and opening, creating,
 * renaming, duplicating and removing scene collections from it. Each
 * collection gets a thumbnail of what it shows, saved next to the collection
 * files in "thumbnails/<file name>.png".
 *
 * LiveMix opens straight into the last document. The documents window only
 * shows at startup when "Show when LiveMix starts" is ticked in it. */

#include "OBSBasic.hpp"

#include <dialogs/OBSWelcome.hpp>
#include <models/SceneCollection.hpp>
#include <utility/ScreenshotObj.hpp>

#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QImage>
#include <QTimer>

#include <filesystem>

extern bool safe_mode;

namespace {
/* A new key, so the old welcome window's "show" setting (on by default) is not carried over */
constexpr const char *kShowWelcomeKey = "LiveMixShowDocuments";
constexpr int kThumbnailIntervalMs = 20000;
const QSize kThumbnailSize(320, 180);

std::filesystem::path collectionsDirectory()
{
	return App()->userScenesLocation / std::filesystem::u8path("obs-studio/basic/scenes");
}

bool hasVisibleContent(obs_scene_t *scene, int depth = 0)
{
	if (!scene || depth > 8) {
		return false;
	}

	struct Ctx {
		bool found;
		int depth;
	} ctx{false, depth};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			Ctx *ctx = static_cast<Ctx *>(param);
			if (!obs_sceneitem_visible(item)) {
				return true;
			}
			obs_source_t *source = obs_sceneitem_get_source(item);
			obs_scene_t *child = obs_scene_from_source(source);
			if (!child) {
				child = obs_group_from_source(source);
			}
			ctx->found = child ? hasVisibleContent(child, ctx->depth + 1) : true;
			return !ctx->found;
		},
		&ctx);
	return ctx.found;
}

/* A name not used by any collection: "Untitled 1", "Untitled 2", ... */
std::string unusedName(const OBSBasic *main, const std::string &base, bool numberFirst)
{
	if (!numberFirst && !main->GetSceneCollectionByName(base)) {
		return base;
	}
	for (int i = numberFirst ? 1 : 2;; i++) {
		const std::string candidate = base + " " + std::to_string(i);
		if (!main->GetSceneCollectionByName(candidate)) {
			return candidate;
		}
	}
}

/* Copies a collection file under a new collection name */
bool copyCollectionFile(const std::filesystem::path &from, const std::filesystem::path &to, const std::string &name)
{
	try {
		std::filesystem::copy(from, to, std::filesystem::copy_options::overwrite_existing);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_WARNING, "Welcome: could not copy scene collection: %s", error.what());
		return false;
	}

	OBSDataAutoRelease data = obs_data_create_from_json_file(to.u8string().c_str());
	if (!data) {
		return false;
	}
	obs_data_set_string(data, "name", name.c_str());
	return obs_data_save_json_safe(data, to.u8string().c_str(), "tmp", nullptr);
}

void moveThumbnail(const std::filesystem::path &from, const std::filesystem::path &to, bool copy)
{
	std::error_code ec;
	if (!std::filesystem::exists(from, ec)) {
		return;
	}
	if (copy) {
		std::filesystem::copy(from, to, std::filesystem::copy_options::overwrite_existing, ec);
	} else {
		std::filesystem::rename(from, to, ec);
	}
}

void welcomeEvent(enum obs_frontend_event event, void *data)
{
	OBSBasic *main = static_cast<OBSBasic *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		/* Give sources a moment to show their first frames */
		QTimer::singleShot(3000, main, [main]() { main->SaveCollectionThumbnail(); });
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_event_callback(welcomeEvent, data);
		break;
	default:
		break;
	}
}
} // namespace

void OBSBasic::SetupWelcome()
{
	config_set_default_bool(App()->GetUserConfig(), "General", kShowWelcomeKey, false);

	obs_frontend_add_event_callback(welcomeEvent, this);

	collectionThumbnailTimer = new QTimer(this);
	collectionThumbnailTimer->setInterval(kThumbnailIntervalMs);
	connect(collectionThumbnailTimer, &QTimer::timeout, this, &OBSBasic::SaveCollectionThumbnail);
	collectionThumbnailTimer->start();

	if (config_get_bool(App()->GetUserConfig(), "General", kShowWelcomeKey) && !safe_mode) {
		QTimer::singleShot(0, this, &OBSBasic::ShowWelcome);
	}
}

void OBSBasic::ShowWelcome()
{
	OBSWelcome welcome(this);
	welcome.exec();
}

std::filesystem::path OBSBasic::CollectionThumbnailPath(const std::string &fileStem) const
{
	return collectionsDirectory() / "thumbnails" / std::filesystem::u8path(fileStem + ".png");
}

void OBSBasic::SaveCollectionThumbnail()
{
	if (isClosing() || disableSaving || !isVisible()) {
		return;
	}

	/* What is live, or the Preview when nothing is */
	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	OBSSourceAutoRelease preview = obs_frontend_preview_program_mode_active()
					       ? obs_frontend_get_current_preview_scene()
					       : nullptr;
	obs_source_t *source = program;
	if (preview && !hasVisibleContent(obs_scene_from_source(program))) {
		source = preview;
	}
	if (!source) {
		return;
	}

	const std::filesystem::path path = CollectionThumbnailPath(GetCurrentSceneCollection().getFileStem());

	ScreenshotObj::Options options;
	options.size = kThumbnailSize;
	options.outputToFile = false;
	ScreenshotObj *shot = new ScreenshotObj(source, options);
	connect(shot, &ScreenshotObj::imageReady, this, [path](QImage image) {
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		image.save(QString::fromStdString(path.u8string()), "PNG");
	});
}

bool OBSBasic::OpenCollection(const std::string &name)
{
	if (name == config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")) {
		return true;
	}

	auto found = GetSceneCollectionByName(name);
	if (!found) {
		return false;
	}

	/* Remember how the document we are leaving looks */
	SaveCollectionThumbnail();

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);
	ActivateSceneCollection(found.value());
	blog(LOG_INFO, "Switched to scene collection '%s' (welcome window)", name.c_str());
	return true;
}

bool OBSBasic::CreateUntitledCollection()
{
	SaveCollectionThumbnail();
	return CreateNewSceneCollection(QT_UTF8(unusedName(this, "Untitled", true).c_str()));
}

bool OBSBasic::OpenCollectionFile(const QString &filePath)
{
	const std::filesystem::path path = std::filesystem::u8path(QT_TO_UTF8(filePath));

	/* Already one of ours */
	if (auto found = GetSceneCollectionByFileName(path.filename().u8string())) {
		std::error_code ec;
		if (std::filesystem::equivalent(found->getFilePath(), path, ec)) {
			return OpenCollection(found->getName());
		}
	}

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(path.u8string().c_str(), "bak");
	if (!data || !obs_data_has_user_value(data, "sources")) {
		OBSMessageBox::warning(this, QTStr("Wirecast.Welcome.OpenFailed.Title"),
				       QTStr("Wirecast.Welcome.OpenFailed.Text").arg(filePath));
		return false;
	}

	std::string name = obs_data_get_string(data, "name");
	if (name.empty()) {
		name = path.stem().u8string();
	}
	name = unusedName(this, name, false);

	try {
		SceneCollection &collection = CreateSceneCollection(name);
		if (!copyCollectionFile(path, collection.getFilePath(), name)) {
			collections.erase(name);
			return false;
		}
	} catch (const std::exception &error) {
		blog(LOG_WARNING, "Welcome: %s", error.what());
		return false;
	}

	RefreshSceneCollections(true);
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	return OpenCollection(name);
}

bool OBSBasic::RenameCollection(const std::string &name, const std::string &newName)
{
	if (newName.empty() || newName == name || GetSceneCollectionByName(newName)) {
		return false;
	}

	auto found = GetSceneCollectionByName(name);
	if (!found) {
		return false;
	}
	const SceneCollection old = found.value();

	try {
		if (name == config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")) {
			SetupRenameSceneCollection(newName);
		} else {
			SceneCollection &collection = CreateSceneCollection(newName);
			if (!copyCollectionFile(old.getFilePath(), collection.getFilePath(), newName)) {
				collections.erase(newName);
				return false;
			}
			RemoveSceneCollection(old);
			collections.erase(name);
			RefreshSceneCollections();
			OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
		}
	} catch (const std::exception &error) {
		blog(LOG_WARNING, "Welcome: %s", error.what());
		return false;
	}

	if (auto renamed = GetSceneCollectionByName(newName)) {
		moveThumbnail(CollectionThumbnailPath(old.getFileStem()),
			      CollectionThumbnailPath(renamed->getFileStem()), false);
	}
	return true;
}

bool OBSBasic::DuplicateCollection(const std::string &name)
{
	auto found = GetSceneCollectionByName(name);
	if (!found) {
		return false;
	}
	const SceneCollection source = found.value();

	if (name == config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")) {
		SaveProjectNow();
	}

	const std::string newName = unusedName(this, name, false);
	try {
		SceneCollection &collection = CreateSceneCollection(newName);
		if (!copyCollectionFile(source.getFilePath(), collection.getFilePath(), newName)) {
			collections.erase(newName);
			return false;
		}
		moveThumbnail(CollectionThumbnailPath(source.getFileStem()),
			      CollectionThumbnailPath(collection.getFileStem()), true);
	} catch (const std::exception &error) {
		blog(LOG_WARNING, "Welcome: %s", error.what());
		return false;
	}

	RefreshSceneCollections();
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	return true;
}

bool OBSBasic::RemoveCollection(const std::string &name)
{
	auto found = GetSceneCollectionByName(name);
	if (!found) {
		return false;
	}
	const SceneCollection collection = found.value();
	const std::filesystem::path thumbnail = CollectionThumbnailPath(collection.getFileStem());

	if (name == config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")) {
		/* Switches to another collection first; refuses the last one */
		if (collections.size() <= 1) {
			return false;
		}
		on_actionRemoveSceneCollection_triggered(true);
	} else {
		try {
			RemoveSceneCollection(collection);
		} catch (const std::exception &error) {
			blog(LOG_WARNING, "Welcome: %s", error.what());
			return false;
		}
		collections.erase(name);
		RefreshSceneCollections();
		OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	}

	std::error_code ec;
	std::filesystem::remove(thumbnail, ec);
	return true;
}
