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

/* Wirecast's Shot Editor: the six sidebar tabs that edit the shot selected in
 * the layer rows.
 *
 *   Shot Layers                   the sources stacked inside the shot
 *   Shot Layer Properties         position, size, fill, rotation, opacity, crop, color
 *   Audio                         volume, pan, meter, mute, monitor
 *   Chroma Key Properties         chroma / luma / color key
 *   Smooth Transition Properties  build in / build out of the layer
 *   Source Properties (...)       the source's own settings
 *
 * A shot is an OBS scene and a shot layer is one of its scene items. Opacity,
 * color and keying are done with filters named "Wirecast Color" and
 * "Wirecast Key" on the layer's source.
 */

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

class QWidget;

class ShotEditor : public QObject {
	Q_OBJECT

public:
	enum class Page {
		Layers,
		LayerProperties,
		Audio,
		ChromaKey,
		SmoothTransition,
		SourceProperties,
	};

	explicit ShotEditor(QObject *parent = nullptr);
	~ShotEditor() override;

	QWidget *createPage(Page page, QWidget *parent);

	OBSSource shot() const;
	OBSScene shotScene() const;
	/* The selected layer of the shot, or null */
	OBSSceneItem item() const;
	void selectItem(obs_sceneitem_t *item);

	/* Title of the Source Properties tab, e.g. "Media Source Properties" */
	QString sourcePageTitle() const;

public slots:
	/* Picks up the shot selected in the layer rows */
	void refreshShot();

private slots:
	void emitLayersChanged();
	void emitTransformed();

private:
	/* Held so the scene's signal handler outlives our connections to it */
	OBSSource heldShot;
	int64_t itemId = 0;
	bool eventCallbackRegistered = false;
	std::vector<OBSSignal> shotSignals;
	OBSSignal renameSignal;
	QTimer layersTimer;
	QTimer transformTimer;

	void setShot(obs_source_t *shot);
	void ensureItem();

	static void sceneChanged(void *data, calldata_t *params);
	static void itemTransformed(void *data, calldata_t *params);
	static void shotRemoved(void *data, calldata_t *params);
	static void sourceRenamed(void *data, calldata_t *params);
	static void onFrontendEvent(enum obs_frontend_event event, void *data);

signals:
	void shotChanged();
	void layersChanged();
	void itemChanged();
	void itemTransformedSignal();
	void sourcePageTitleChanged(const QString &title);
};
