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

/* Wirecast-style shot layers.
 *
 * Every scene is a "shot" that lives on one of kLayerCount layer rows. Clicking
 * a shot makes it the active shot of its layer. The active shots of all layers
 * are stacked (layer 1 on top) as nested scenes inside a single "Layer Mix"
 * scene, which is sent to Preview (Studio Mode) or Program. With AutoLive on,
 * a click also takes the result live.
 *
 * State is kept in the scene collection itself:
 *   - scene private setting  "wc_layer"     : layer a shot belongs to (1-based)
 *   - scene private setting  "wc_hotkey"    : number key that takes the shot (0 = none)
 *   - scene private setting  "wc_icon"      : locked shot icon (base64 PNG)
 *   - scene private setting  "wc_dup_copy"  : sources copied rather than shared on duplicate
 *   - scene private setting  "wc_layer_mix" : marks the Layer Mix scene
 *   - mix private setting    "wc_hidden"    : bitmask of hidden layers
 *   - item private setting   "wc_layer"     : layer an item in the mix shows
 *
 * Several panels can be open at once (the main one plus "New Layer Window"
 * windows that show a single layer); the selected shot, current layer and
 * AutoLive are shared between them.
 */

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QFrame>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QMenu;
class QScrollArea;
class QVBoxLayout;
class ThumbnailView;

class ShotTile : public QFrame {
	Q_OBJECT

	OBSWeakSource weakScene;
	int layer;
	bool clearTile;
	bool active = false;
	bool live = false;
	bool layerHidden = false;
	int zoom = 100;
	int hotkey = 0;
	bool iconLocked = false;

	QLabel *thumbnailLabel = nullptr;
	QLabel *nameLabel = nullptr;
	QLabel *hotkeyLabel = nullptr;
	QPointer<ThumbnailView> thumbnail;
	QPixmap pixmap;
	QString fullName;

	QPoint dragStartPos;
	bool pressed = false;

	void updateClasses();
	void updateThumbnailLabel();
	void applyZoom();

public:
	/* A null scene creates the "Clear" tile that empties the layer */
	ShotTile(QWidget *parent, obs_source_t *scene, int layer, int zoom);

	OBSSource getScene() const;
	int getLayer() const { return layer; }
	bool isClearTile() const { return clearTile; }
	QPixmap currentPixmap() const { return pixmap; }

	void setActive(bool active);
	void setLive(bool live);
	void setLayerHidden(bool hidden);
	void setName(const QString &name);
	void setZoom(int percent);
	void setHotkey(int key);
	void setLockedIcon(const QPixmap &icon);
	void requestThumbnailUpdate();

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
	void setPixmap(QPixmap pixmap);

signals:
	void clicked();
	void doubleClicked();
	void menuRequested(const QPoint &globalPos);
};

class LayerRow : public QFrame {
	Q_OBJECT

	int layer;
	QLabel *header = nullptr;
	QHBoxLayout *tileLayout = nullptr;

public:
	LayerRow(QWidget *parent, int layer);

	int getLayer() const { return layer; }
	void addTile(ShotTile *tile);
	void setState(bool hidden, bool current);

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

signals:
	void shotDropped(const QString &uuid, int layer);
	void menuRequested(int layer, const QPoint &globalPos);
};

class LayersPanel : public QFrame {
	Q_OBJECT

public:
	static constexpr int kLayerCount = 5;
	static constexpr int kMinZoom = 50;
	static constexpr int kMaxZoom = 200;

	/* onlyLayer > 0 shows just that layer (Window > New Layer Window) */
	explicit LayersPanel(QWidget *parent = nullptr, int onlyLayer = 0);
	~LayersPanel() override;

	/* ---- Selection shared by all panels (the shot Edit-menu commands act on) ---- */
	static OBSSource selectedShot();
	static int currentLayer();

	/* ---- Switching ---- */
	static bool autoLive();
	static void setAutoLive(bool enabled);
	void takeShotsWithHotkey(int key);

	static bool isLayerVisible(int layer);
	void setLayerVisible(int layer, bool visible);
	void goToLayer(int layer);

	/* ---- Shot commands (Edit menu and the shot's right-click menu) ---- */
	void editShot(obs_source_t *scene);
	void renameShot(obs_source_t *scene);
	void duplicateShot(obs_source_t *scene);
	void deleteShot(obs_source_t *scene);
	void setShotLayer(obs_source_t *scene, int layer);
	void fillMoveToLayerMenu(QMenu *menu, obs_source_t *scene);
	void fillDuplicateOptionsMenu(QMenu *menu, obs_source_t *scene);
	void addShotMenuItems(QMenu *menu, obs_source_t *scene, ShotTile *tile = nullptr);

	/* ---- New shots ---- */
	void importMedia(const QStringList &files);
	/* setupItem places each new item; without it the sources are fitted to the canvas */
	OBSSource createShot(const QString &baseName, const std::vector<OBSSource> &sources,
			     const std::function<void(obs_sceneitem_t *item, size_t index)> &setupItem = nullptr);
	void createReplayShot(const QString &file, bool updateExisting);

	/* ---- Layer zoom: Layout > Zoom Layers, or drag the line under a layer ---- */
	int layerZoom(int layer) const;
	void setLayerZoom(int layer, int percent);
	void setAllLayersZoom(int percent);
	void zoomAllLayers(int stepPercent);

	static void refreshAll();

private:
	static std::vector<LayersPanel *> panels;

	int onlyLayer = 0;
	std::vector<OBSSignal> signalHandlers;
	std::vector<LayerRow *> rows;
	std::vector<ShotTile *> tiles;
	std::vector<int> zooms;
	QScrollArea *scroll = nullptr;

	QTimer rebuildTimer;
	QTimer thumbnailTimer;
	QTimer strayTimer;
	size_t nextThumbnail = 0;
	bool collectionLoading = true;
	bool eventCallbackRegistered = false;
	OBSWeakSource weakMixScene;

	bool showsLayer(int layer) const { return onlyLayer == 0 || onlyLayer == layer; }
	LayerRow *rowFor(int layer) const;

	OBSScene findMixScene();
	OBSScene createMixScene();
	void showMixScene(obs_scene_t *mix);

	static void obsSceneListChanged(void *data, calldata_t *params);
	static void obsSceneActiveChanged(void *data, calldata_t *params);
	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	void clearAllTiles();
	void rebuild();
	void refreshStates();
	void updateNextThumbnail();
	void loadZooms();
	void saveZooms();

	/* Sources dropped or added straight into the hidden Layer Mix (e.g. by
	 * dragging a file onto the Preview) become a shot of their own, so they
	 * show up in the layer rows and can be cleared */
	void adoptStrayItems();

	void selectShot(obs_source_t *scene, int layer);
	void activateShot(int layer, obs_source_t *scene, bool allowAutoLive = true);
	void goLiveIfAutoLive();

	void setShotHotkey(obs_source_t *scene, int key);
	void unmapAllHotkeys();
	void lockShotIcon(obs_source_t *scene, ShotTile *tile, bool lock);
	void changeShotSource(obs_source_t *scene, obs_source_t *source, bool audio);

	void showTileMenu(ShotTile *tile, const QPoint &globalPos);
	void showRowMenu(int layer, const QPoint &globalPos);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

public slots:
	void scheduleRebuild();

signals:
	void autoLiveChanged(bool enabled);
	/* The selected shot may have changed (see selectedShot()) */
	void shotSelectionChanged();
};
