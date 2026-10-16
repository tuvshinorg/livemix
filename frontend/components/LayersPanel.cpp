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

#include "LayersPanel.hpp"
#include "WirecastIcons.hpp"

#include <OBSApp.hpp>
#include <dialogs/NameDialog.hpp>
#include <utility/ThumbnailManager.hpp>
#include <utility/ThumbnailView.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QBuffer>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <cmath>
#include <set>
#include <string>

#include "moc_LayersPanel.cpp"

namespace {
constexpr const char *kLayerKey = "wc_layer";
constexpr const char *kMixKey = "wc_layer_mix";
constexpr const char *kHotkeyKey = "wc_hotkey";
constexpr const char *kIconKey = "wc_icon";
constexpr const char *kDupCopyKey = "wc_dup_copy";
constexpr const char *kHiddenKey = "wc_hidden";
constexpr const char *kReplayKey = "wc_replay";
constexpr const char *kShotMimeType = "application/x-obs-layer-shot";
constexpr const char *kConfigSection = "Wirecast";

constexpr int kThumbWidth = 128;
constexpr int kThumbHeight = 72;
/* Name (and Live / Preview) under the picture */
constexpr int kCaptionHeight = 18;
constexpr int kCaptionGap = 4;

/* Tally outline colours */
const QColor kOutlineIdle(0x2C, 0x2E, 0x33);
const QColor kOutlineHover(0x55, 0x57, 0x5D);
const QColor kOutlineClear(0x4A, 0x4C, 0x52);

/* Shared by every panel */
OBSWeakSource selectedScene;
int selectedLayer = 1;
bool autoLiveLoaded = false;
bool autoLiveEnabled = false;

bool isMixScene(obs_source_t *source)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	return obs_data_get_bool(priv, kMixKey);
}

int getShotLayer(obs_source_t *source)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	long long layer = obs_data_get_int(priv, kLayerKey);
	return (int)std::clamp(layer, 1LL, (long long)LayersPanel::kLayerCount);
}

int getShotHotkey(obs_source_t *source)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	return (int)obs_data_get_int(priv, kHotkeyKey);
}

/* Returns 0 for items that were not placed by the layers panel */
int getItemLayer(obs_sceneitem_t *item)
{
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
	return (int)obs_data_get_int(priv, kLayerKey);
}

std::vector<OBSSource> getAllScenes()
{
	std::vector<OBSSource> result;

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		result.emplace_back(scenes.sources.array[i]);
	}
	obs_frontend_source_list_free(&scenes);

	return result;
}

std::vector<OBSSource> getShotScenes()
{
	std::vector<OBSSource> shots = getAllScenes();
	shots.erase(std::remove_if(shots.begin(), shots.end(),
				   [](const OBSSource &source) { return isMixScene(source); }),
		    shots.end());
	return shots;
}

OBSSource findMixSceneSource()
{
	for (const OBSSource &scene : getAllScenes()) {
		if (isMixScene(scene)) {
			return scene;
		}
	}
	return nullptr;
}

int hiddenLayerMask()
{
	OBSSource mix = findMixSceneSource();
	if (!mix) {
		return 0;
	}
	OBSDataAutoRelease priv = obs_source_get_private_settings(mix);
	return (int)obs_data_get_int(priv, kHiddenKey);
}

std::vector<OBSSceneItem> getSceneItems(obs_scene_t *scene)
{
	std::vector<OBSSceneItem> items;

	auto addItem = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
		return true;
	};
	obs_scene_enum_items(scene, addItem, &items);

	return items;
}

/* Layer 1 renders on top, higher layers below it, foreign items at the bottom */
void reorderMixScene(obs_scene_t *mix)
{
	std::vector<OBSSceneItem> items = getSceneItems(mix);
	if (items.size() < 2) {
		return;
	}

	auto sortKey = [](obs_sceneitem_t *item) {
		int layer = getItemLayer(item);
		return layer > 0 ? layer : INT_MAX;
	};
	std::stable_sort(items.begin(), items.end(), [&](const OBSSceneItem &a, const OBSSceneItem &b) {
		return sortKey(a) > sortKey(b);
	});

	std::vector<obs_sceneitem_t *> order;
	order.reserve(items.size());
	for (const OBSSceneItem &item : items) {
		order.push_back(item);
	}

	obs_scene_reorder_items(mix, order.data(), order.size());
}

/* "Clear layer" icon: an empty circle with a slash, in the muted icon colour */
QPixmap createClearLayerIcon(int size, qreal ratio)
{
	QPixmap icon(QSize(size, size) * ratio);
	icon.setDevicePixelRatio(ratio);
	icon.fill(Qt::transparent);

	QPainter p(&icon);
	p.setRenderHint(QPainter::Antialiasing);
	p.scale(size / 24.0, size / 24.0);
	p.setPen(QPen(QColor(0x75, 0x72, 0x6B), 1.6, Qt::SolidLine, Qt::RoundCap));
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(QPointF(12, 12), 8.0, 8.0);
	p.drawLine(QPointF(6.5, 17.5), QPointF(17.5, 6.5));
	return icon;
}

constexpr int kTileRadius = 6;

bool isRegularScene(obs_source_t *source)
{
	return source && obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(source);
}

QString uniqueSourceName(const QString &base)
{
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!existing) {
			return name;
		}
		name = QString("%1 %2").arg(base).arg(i);
	}
}

/* Inputs for Change Source (has video) or Change Audio (audio only) */
std::vector<OBSSource> getInputs(bool audioOnly)
{
	std::vector<OBSSource> result;

	auto collect = [](void *param, obs_source_t *source) {
		static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		return true;
	};
	std::vector<OBSSource> all;
	obs_enum_sources(collect, &all);

	for (const OBSSource &source : all) {
		const uint32_t flags = obs_source_get_output_flags(source);
		const bool video = (flags & OBS_SOURCE_VIDEO) != 0;
		const bool audio = (flags & OBS_SOURCE_AUDIO) != 0;
		if (audioOnly ? (audio && !video) : video) {
			result.push_back(source);
		}
	}

	std::sort(result.begin(), result.end(), [](const OBSSource &a, const OBSSource &b) {
		return QString::localeAwareCompare(QT_UTF8(obs_source_get_name(a)), QT_UTF8(obs_source_get_name(b))) <
		       0;
	});
	return result;
}

bool isAudioOnly(obs_source_t *source)
{
	const uint32_t flags = obs_source_get_output_flags(source);
	return (flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_VIDEO);
}

bool hasVideo(obs_source_t *source)
{
	return (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0;
}

/* The item Change Source / Change Audio replaces: the bottom-most video or audio-only item */
obs_sceneitem_t *findShotItem(obs_scene_t *scene, bool audio)
{
	for (const OBSSceneItem &item : getSceneItems(scene)) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (audio ? isAudioOnly(source) : hasVideo(source)) {
			return item;
		}
	}
	return nullptr;
}

void fitItemToCanvas(obs_sceneitem_t *item)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	info.pos = {0.0f, 0.0f};
	info.rot = 0.0f;
	info.scale = {1.0f, 1.0f};
	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.bounds = {float(ovi.base_width), float(ovi.base_height)};
	obs_sceneitem_set_info2(item, &info);
}

/* Swaps the source an item shows, keeping its position, crop, visibility and stacking */
obs_sceneitem_t *replaceItemSource(obs_scene_t *scene, obs_sceneitem_t *item, obs_source_t *source)
{
	obs_transform_info info;
	obs_sceneitem_crop crop;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_get_crop(item, &crop);
	const bool visible = obs_sceneitem_visible(item);
	const int order = obs_sceneitem_get_order_position(item);
	const obs_blending_type blend = obs_sceneitem_get_blending_mode(item);
	const obs_scale_type scaleFilter = obs_sceneitem_get_scale_filter(item);

	obs_sceneitem_t *replacement = obs_scene_add(scene, source);
	if (!replacement) {
		return nullptr;
	}
	obs_sceneitem_remove(item);

	obs_sceneitem_set_info2(replacement, &info);
	obs_sceneitem_set_crop(replacement, &crop);
	obs_sceneitem_set_visible(replacement, visible);
	obs_sceneitem_set_blending_mode(replacement, blend);
	obs_sceneitem_set_scale_filter(replacement, scaleFilter);
	obs_sceneitem_set_order_position(replacement, order);
	obs_sceneitem_select(replacement, false);
	return replacement;
}

QStringList getDupCopyNames(obs_source_t *scene)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	const QString value = QT_UTF8(obs_data_get_string(priv, kDupCopyKey));
	return value.isEmpty() ? QStringList() : value.split('\n');
}

void setDupCopyNames(obs_source_t *scene, const QStringList &names)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	obs_data_set_string(priv, kDupCopyKey, QT_TO_UTF8(names.join('\n')));
}

QPixmap decodeIcon(obs_source_t *scene)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	const char *data = obs_data_get_string(priv, kIconKey);
	QPixmap icon;
	if (data && *data) {
		icon.loadFromData(QByteArray::fromBase64(QByteArray(data)), "PNG");
	}
	return icon;
}

QString withShortcut(const QString &text, const char *shortcut)
{
	/* Text after a tab is drawn in the menu's shortcut column */
	return text + "\t" + QKeySequence(QString::fromLatin1(shortcut)).toString(QKeySequence::NativeText);
}

/* The line under each layer: drag to make that layer taller or shorter, Ctrl+drag for all */
class LayerDivider : public QFrame {
public:
	std::function<void()> dragStarted;
	std::function<void(int dy, bool allLayers)> dragged;

	explicit LayerDivider(QWidget *parent) : QFrame(parent)
	{
		setObjectName("layerDivider");
		setFixedHeight(5);
		setCursor(Qt::SizeVerCursor);
		setToolTip(QTStr("Wirecast.Layers.Divider.Tooltip"));
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			dragging = true;
			startY = event->globalPosition().y();
			if (dragStarted) {
				dragStarted();
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (dragging && dragged) {
			dragged(int(event->globalPosition().y() - startY),
				(event->modifiers() & Qt::ControlModifier) != 0);
		}
	}

	void mouseReleaseEvent(QMouseEvent *) override { dragging = false; }

private:
	bool dragging = false;
	double startY = 0.0;
};
} // namespace

/* ------------------------------------------------------------------------- */
/* ShotTile                                                                  */

ShotTile::ShotTile(QWidget *parent, obs_source_t *scene, int layer, int zoom_)
	: QFrame(parent),
	  layer(layer),
	  clearTile(scene == nullptr),
	  zoom(zoom_)
{
	setAttribute(Qt::WA_Hover);
	setCursor(Qt::PointingHandCursor);

	/* Shot button: a rounded 16:9 picture with its tally outline painted in the
	 * margin around it, and the name underneath */
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(kOutlineMargin, kOutlineMargin, kOutlineMargin, 0);
	layout->setSpacing(kCaptionGap);

	thumbnailLabel = new QLabel(this);
	thumbnailLabel->setObjectName("shotThumbnail");
	thumbnailLabel->setAlignment(Qt::AlignCenter);
	layout->addWidget(thumbnailLabel);

	/* The number key assigned with Keyboard Shortcut, drawn on the picture */
	hotkeyLabel = new QLabel(thumbnailLabel);
	hotkeyLabel->setObjectName("shotHotkey");
	hotkeyLabel->setAlignment(Qt::AlignCenter);
	hotkeyLabel->move(5, 5);
	hotkeyLabel->hide();

	QHBoxLayout *caption = new QHBoxLayout();
	caption->setContentsMargins(0, 0, 0, 0);
	caption->setSpacing(6);

	nameLabel = new QLabel(this);
	nameLabel->setObjectName("shotName");
	nameLabel->setFixedHeight(kCaptionHeight);
	nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	/* Long names are elided; they must never widen the tile */
	nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	caption->addWidget(nameLabel, 1);

	stateLabel = new QLabel(this);
	stateLabel->setObjectName("shotState");
	stateLabel->setFixedHeight(kCaptionHeight);
	stateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	stateLabel->hide();
	caption->addWidget(stateLabel);
	layout->addLayout(caption);

	applyZoom();

	if (clearTile) {
		setName(QTStr("Basic.Layers.Clear"));
		setToolTip(QTStr("Basic.Layers.Clear.Tooltip").arg(layer));
	} else {
		weakScene = OBSGetWeakRef(scene);
		setName(QT_UTF8(obs_source_get_name(scene)));

		thumbnail = App()->thumbnails()->createView(this, scene);
		connect(thumbnail, &ThumbnailView::updated, this, &ShotTile::setPixmap);
		setPixmap(thumbnail->getPixmap());
	}

	updateClasses();
}

OBSSource ShotTile::getScene() const
{
	return OBSGetStrongRef(weakScene);
}

QSize ShotTile::thumbnailSize(int zoom)
{
	return QSize(std::max(kThumbWidth * zoom / 100, 40), std::max(kThumbHeight * zoom / 100, 22));
}

void ShotTile::setName(const QString &name)
{
	fullName = name;
	updateNameText();
	setToolTip(name);
}

void ShotTile::updateNameText()
{
	/* The name gets whatever the Live / Preview word leaves */
	int room = thumbnailLabel->width() - 4;
	if (!stateLabel->text().isEmpty()) {
		room -= QFontMetrics(stateLabel->font()).horizontalAdvance(stateLabel->text()) + 10;
	}
	QFontMetrics metrics(nameLabel->font());
	nameLabel->setText(metrics.elidedText(fullName, Qt::ElideRight, std::max(room, 12)));
}

void ShotTile::applyZoom()
{
	const QSize thumb = thumbnailSize(zoom);
	const int w = thumb.width();
	const int h = thumb.height();
	thumbnailLabel->setFixedSize(w, h);
	setFixedSize(w + kOutlineMargin * 2, h + kOutlineMargin + kCaptionGap + kCaptionHeight);

	if (clearTile) {
		thumbnailLabel->setPixmap(createClearLayerIcon(std::clamp(h / 3, 14, 48), devicePixelRatioF()));
		if (!fullName.isEmpty()) {
			const QString tip = toolTip();
			setName(fullName);
			setToolTip(tip);
		}
	} else {
		if (!fullName.isEmpty()) {
			setName(fullName);
		}
		updateThumbnailLabel();
	}
}

void ShotTile::setZoom(int percent)
{
	if (zoom != percent) {
		zoom = percent;
		applyZoom();
	}
}

void ShotTile::setHotkey(int key)
{
	hotkey = key;
	if (key > 0 && key <= 10) {
		hotkeyLabel->setText(QString::number(key % 10));
		hotkeyLabel->adjustSize();
		hotkeyLabel->show();
	} else {
		hotkeyLabel->hide();
	}
}

void ShotTile::setLockedIcon(const QPixmap &icon)
{
	iconLocked = !icon.isNull();
	if (iconLocked) {
		pixmap = icon;
		updateThumbnailLabel();
	} else if (thumbnail) {
		pixmap = thumbnail->getPixmap();
		updateThumbnailLabel();
	}
}

void ShotTile::setActive(bool active_)
{
	if (active != active_) {
		active = active_;
		updateClasses();
	}
}

void ShotTile::setLive(bool live_)
{
	if (live != live_) {
		live = live_;
		updateClasses();
	}
}

void ShotTile::setLayerHidden(bool hidden)
{
	if (layerHidden != hidden) {
		layerHidden = hidden;
		updateClasses();
	}
}

void ShotTile::updateClasses()
{
	QString classes = clearTile ? "shot-tile shot-clear" : "shot-tile";
	if (active) {
		classes += " shot-active";
	}
	if (live) {
		classes += " shot-live";
	}
	if (layerHidden) {
		classes += " shot-hidden";
	}
	setClasses(this, classes);

	/* Live wins when a shot is both live and in Preview; the double outline shows both */
	const QString state = live ? QTStr("Wirecast.Shot.StateLive")
				   : (active ? QTStr("Wirecast.Shot.StatePreview") : QString());
	stateLabel->setText(state);
	stateLabel->setVisible(!state.isEmpty());
	updateNameText();
	update();
}

bool ShotTile::event(QEvent *event)
{
	if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) {
		update();
	}
	return QFrame::event(event);
}

void ShotTile::paintEvent(QPaintEvent *event)
{
	QFrame::paintEvent(event);

	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setBrush(Qt::NoBrush);

	const QRectF picture = QRectF(thumbnailLabel->geometry());
	auto outline = [&](qreal outset, qreal width, const QColor &color, Qt::PenStyle style = Qt::SolidLine) {
		/* The pen is centred on the path, so push it out by half its width */
		const qreal grow = outset + width / 2.0;
		QPen pen(color, width, style);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.drawRoundedRect(picture.adjusted(-grow, -grow, grow, grow), kTileRadius + grow, kTileRadius + grow);
	};

	const bool hover = underMouse();
	if (live && active) {
		outline(0.0, 2.0, WirecastIcons::kRed);
		outline(3.5, 1.5, WirecastIcons::kGreen);
	} else if (live) {
		outline(0.0, 2.0, WirecastIcons::kRed);
	} else if (active) {
		outline(0.0, 2.0, WirecastIcons::kGreen);
	} else if (clearTile) {
		outline(-1.0, 1.0, hover ? kOutlineHover : kOutlineClear, Qt::DashLine);
	} else {
		outline(0.0, 1.0, hover ? kOutlineHover : kOutlineIdle);
	}
}

void ShotTile::requestThumbnailUpdate()
{
	if (thumbnail && !iconLocked) {
		thumbnail->requestUpdate();
	}
}

void ShotTile::setPixmap(QPixmap pixmap_)
{
	if (iconLocked) {
		return;
	}
	pixmap = std::move(pixmap_);
	updateThumbnailLabel();
}

void ShotTile::updateThumbnailLabel()
{
	if (clearTile) {
		return;
	}

	if (pixmap.isNull()) {
		thumbnailLabel->clear();
		return;
	}

	qreal ratio = devicePixelRatioF();
	QPixmap scaled =
		pixmap.scaled(thumbnailLabel->size() * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	scaled.setDevicePixelRatio(ratio);

	/* Round every corner; the tally outline is painted around it by the tile */
	QPixmap rounded(thumbnailLabel->size() * ratio);
	rounded.setDevicePixelRatio(ratio);
	rounded.fill(Qt::transparent);
	{
		const QSizeF size = thumbnailLabel->size();
		QPainterPath clip;
		clip.addRoundedRect(QRectF(QPointF(0, 0), size), kTileRadius, kTileRadius);
		QPainter p(&rounded);
		p.setRenderHint(QPainter::Antialiasing);
		p.setClipPath(clip);
		p.fillPath(clip, QColor(0x15, 0x16, 0x18));
		const QSizeF picture = QSizeF(scaled.size()) / ratio;
		p.drawPixmap(QPointF((size.width() - picture.width()) / 2, (size.height() - picture.height()) / 2),
			     scaled);
	}
	thumbnailLabel->setPixmap(rounded);
}

void ShotTile::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		dragStartPos = event->position().toPoint();
		pressed = true;
	}
	event->accept();
}

void ShotTile::mouseMoveEvent(QMouseEvent *event)
{
	if (!pressed || clearTile || !(event->buttons() & Qt::LeftButton)) {
		return;
	}

	if ((event->position().toPoint() - dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
		return;
	}

	pressed = false;

	OBSSource scene = getScene();
	if (!scene) {
		return;
	}

	QMimeData *mime = new QMimeData();
	mime->setData(kShotMimeType, QByteArray(obs_source_get_uuid(scene)));

	QDrag *drag = new QDrag(this);
	drag->setMimeData(mime);
	drag->setPixmap(grab());
	drag->setHotSpot(event->position().toPoint());
	drag->exec(Qt::MoveAction);
}

void ShotTile::mouseReleaseEvent(QMouseEvent *event)
{
	bool wasPressed = pressed;
	pressed = false;

	if (wasPressed && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
		emit clicked();
	}
}

void ShotTile::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		emit doubleClicked();
	}
}

void ShotTile::contextMenuEvent(QContextMenuEvent *event)
{
	emit menuRequested(event->globalPos());
	event->accept();
}

/* ------------------------------------------------------------------------- */
/* LayerRow                                                                  */

LayerRow::LayerRow(QWidget *parent, int layer) : QFrame(parent), layer(layer)
{
	setAcceptDrops(true);
	setClasses(this, "layer-row");

	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/* Each row is labelled with its layer */
	header = new QLabel(QTStr("Basic.Layers.Layer").arg(layer), this);
	header->setObjectName("layerHeader");
	header->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	header->setFixedWidth(76);
	header->setToolTip(QTStr("Basic.Layers.Layer").arg(layer));
	layout->addWidget(header);

	QWidget *tileArea = new QWidget(this);
	tileArea->setObjectName("layerTiles");
	tileLayout = new QHBoxLayout(tileArea);
	tileLayout->setContentsMargins(0, 5, 8, 5);
	tileLayout->setSpacing(8);

	/* "Add shot": same size as a shot picture, opens this row's menu */
	addShotHolder = new QWidget(tileArea);
	QVBoxLayout *holderLayout = new QVBoxLayout(addShotHolder);
	holderLayout->setContentsMargins(ShotTile::kOutlineMargin, ShotTile::kOutlineMargin, ShotTile::kOutlineMargin, 0);
	holderLayout->setSpacing(0);
	QToolButton *addButton = new QToolButton(addShotHolder);
	addButton->setIcon(WirecastIcons::icon(WirecastIcons::Glyph::Plus, 14));
	addButton->setIconSize(QSize(14, 14));
	addButton->setText(QTStr("Wirecast.Layers.AddShotTile"));
	addButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	addButton->setToolTip(QTStr("Basic.Layers.AddShot"));
	addButton->setCursor(Qt::PointingHandCursor);
	setClasses(addButton, "wc-add-shot");
	connect(addButton, &QToolButton::clicked, this, [this, addButton]() {
		emit menuRequested(this->layer, addButton->mapToGlobal(QPoint(0, addButton->height())));
	});
	holderLayout->addWidget(addButton);
	holderLayout->addStretch();
	addShotButton = addButton;
	tileLayout->addWidget(addShotHolder);

	tileLayout->addStretch();
	layout->addWidget(tileArea, 1);
	setZoom(100);
}

void LayerRow::addTile(ShotTile *tile)
{
	/* Before "Add shot" and the stretch */
	tileLayout->insertWidget(tileLayout->indexOf(addShotHolder), tile);
}

void LayerRow::setZoom(int percent)
{
	const QSize thumb = ShotTile::thumbnailSize(percent);
	addShotButton->setFixedSize(thumb);
	addShotHolder->setFixedWidth(thumb.width() + ShotTile::kOutlineMargin * 2);
}

void LayerRow::setState(bool hidden, bool current)
{
	QString classes = "layer-row";
	if (hidden) {
		classes += " layer-hidden";
	}
	if (current) {
		classes += " layer-current";
	}
	setProperty("baseClasses", classes);
	setClasses(this, classes);

	QString tip = QTStr("Basic.Layers.Layer").arg(layer);
	if (hidden) {
		tip += " - " + QTStr("Wirecast.Layers.Hidden");
	}
	header->setToolTip(tip);
}

void LayerRow::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat(kShotMimeType)) {
		setClasses(this, property("baseClasses").toString() + " drop-target");
		event->acceptProposedAction();
	}
}

void LayerRow::dragLeaveEvent(QDragLeaveEvent *)
{
	setClasses(this, property("baseClasses").toString());
}

void LayerRow::dropEvent(QDropEvent *event)
{
	setClasses(this, property("baseClasses").toString());

	if (!event->mimeData()->hasFormat(kShotMimeType)) {
		return;
	}

	event->acceptProposedAction();
	emit shotDropped(QString::fromUtf8(event->mimeData()->data(kShotMimeType)), layer);
}

void LayerRow::contextMenuEvent(QContextMenuEvent *event)
{
	emit menuRequested(layer, event->globalPos());
	event->accept();
}

/* ------------------------------------------------------------------------- */
/* LayersPanel                                                               */

std::vector<LayersPanel *> LayersPanel::panels;

LayersPanel::LayersPanel(QWidget *parent, int onlyLayer_) : QFrame(parent), onlyLayer(onlyLayer_)
{
	panels.push_back(this);
	zooms.assign(kLayerCount, 100);
	loadZooms();

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	QWidget *content = new QWidget();
	content->setObjectName("layersContent");
	QVBoxLayout *rowsLayout = new QVBoxLayout(content);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(0);

	for (int layer = 1; layer <= kLayerCount; layer++) {
		LayerRow *row = new LayerRow(content, layer);
		row->setZoom(zooms[layer - 1]);
		rowsLayout->addWidget(row);
		rows.push_back(row);

		LayerDivider *divider = new LayerDivider(content);
		rowsLayout->addWidget(divider);

		if (!showsLayer(layer)) {
			row->hide();
			divider->hide();
		}

		connect(row, &LayerRow::shotDropped, this, [this](const QString &uuid, int layer) {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (scene) {
				setShotLayer(scene, layer);
			}
		});
		connect(row, &LayerRow::menuRequested, this, &LayersPanel::showRowMenu);

		auto startZoom = std::make_shared<std::vector<int>>();
		divider->dragStarted = [this, startZoom]() { *startZoom = zooms; };
		divider->dragged = [this, layer, startZoom](int dy, bool allLayers) {
			if (startZoom->empty()) {
				return;
			}
			/* The thumbnail grows by exactly the distance dragged */
			const int delta = int(std::lround(dy * 100.0 / kThumbHeight));
			if (allLayers) {
				for (int l = 1; l <= kLayerCount; l++) {
					setLayerZoom(l, (*startZoom)[l - 1] + delta);
				}
			} else {
				setLayerZoom(layer, (*startZoom)[layer - 1] + delta);
			}
		};
	}
	rowsLayout->addStretch();

	scroll->setWidget(content);
	outer->addWidget(scroll);

	rebuildTimer.setSingleShot(true);
	rebuildTimer.setInterval(50);
	connect(&rebuildTimer, &QTimer::timeout, this, &LayersPanel::rebuild);

	thumbnailTimer.setInterval(400);
	connect(&thumbnailTimer, &QTimer::timeout, this, &LayersPanel::updateNextThumbnail);

	/* Only the main panel tidies the Layer Mix, not the Layer windows */
	if (onlyLayer == 0) {
		strayTimer.setInterval(500);
		connect(&strayTimer, &QTimer::timeout, this, &LayersPanel::adoptStrayItems);
		strayTimer.start();
	}

	signal_handler_t *sh = obs_get_signal_handler();
	signalHandlers.emplace_back(sh, "source_create", &LayersPanel::obsSceneListChanged, this);
	signalHandlers.emplace_back(sh, "source_remove", &LayersPanel::obsSceneListChanged, this);
	signalHandlers.emplace_back(sh, "source_rename", &LayersPanel::obsSceneListChanged, this);
	signalHandlers.emplace_back(sh, "source_activate", &LayersPanel::obsSceneActiveChanged, this);
	signalHandlers.emplace_back(sh, "source_deactivate", &LayersPanel::obsSceneActiveChanged, this);

	obs_frontend_add_event_callback(&LayersPanel::onFrontendEvent, this);
	eventCallbackRegistered = true;

	/* A layer window opened after loading builds straight away */
	if (onlyLayer > 0) {
		collectionLoading = false;
		scheduleRebuild();
	}
}

LayersPanel::~LayersPanel()
{
	panels.erase(std::remove(panels.begin(), panels.end(), this), panels.end());
	signalHandlers.clear();

	if (eventCallbackRegistered) {
		obs_frontend_remove_event_callback(&LayersPanel::onFrontendEvent, this);
	}
}

LayerRow *LayersPanel::rowFor(int layer) const
{
	return (layer >= 1 && layer <= kLayerCount) ? rows[layer - 1] : nullptr;
}

void LayersPanel::refreshAll()
{
	for (LayersPanel *panel : panels) {
		panel->scheduleRebuild();
	}
}

/* ---- Selection ---------------------------------------------------------- */

OBSSource LayersPanel::selectedShot()
{
	OBSSource scene = OBSGetStrongRef(selectedScene);
	if (scene && !obs_source_removed(scene)) {
		return scene;
	}

	/* Nothing clicked yet: the shot showing on the current layer */
	if (OBSSource mix = findMixSceneSource()) {
		for (const OBSSceneItem &item : getSceneItems(obs_scene_from_source(mix))) {
			if (getItemLayer(item) == selectedLayer) {
				return obs_sceneitem_get_source(item);
			}
		}
	}
	return nullptr;
}

int LayersPanel::currentLayer()
{
	return selectedLayer;
}

void LayersPanel::adoptStrayItems()
{
	if (onlyLayer != 0 || collectionLoading) {
		return;
	}

	OBSScene mix = findMixScene();
	if (!mix) {
		return;
	}

	std::vector<OBSSceneItem> stray;
	std::vector<bool> layerUsed(kLayerCount + 1, false);
	for (const OBSSceneItem &item : getSceneItems(mix)) {
		if (isRegularScene(obs_sceneitem_get_source(item))) {
			const int layer = getItemLayer(item);
			if (layer >= 1 && layer <= kLayerCount) {
				layerUsed[layer] = true;
			}
		} else {
			stray.push_back(item);
		}
	}
	if (stray.empty()) {
		return;
	}

	/* An empty layer, so the shots already showing stay where they are */
	int target = selectedLayer;
	for (int i = 0; i < kLayerCount; i++) {
		const int layer = (selectedLayer - 1 + i) % kLayerCount + 1;
		if (!layerUsed[layer]) {
			target = layer;
			break;
		}
	}

	/* getSceneItems lists the bottom first, which keeps the stacking order */
	std::vector<OBSSource> sources;
	for (const OBSSceneItem &item : stray) {
		sources.emplace_back(obs_sceneitem_get_source(item));
	}

	selectedLayer = target;
	OBSSource shot = createShot(QT_UTF8(obs_source_get_name(sources.front())), sources,
				    [&stray](obs_sceneitem_t *item, size_t index) {
					    /* Keep the placement it had in the mix */
					    obs_transform_info info;
					    obs_sceneitem_crop crop;
					    obs_sceneitem_get_info2(stray[index], &info);
					    obs_sceneitem_get_crop(stray[index], &crop);
					    obs_sceneitem_set_info2(item, &info);
					    obs_sceneitem_set_crop(item, &crop);
					    obs_sceneitem_set_visible(item, obs_sceneitem_visible(stray[index]));
				    });

	for (const OBSSceneItem &item : stray) {
		obs_sceneitem_remove(item);
	}

	blog(LOG_INFO, "Layers: moved %zu source(s) from the Layer Mix into shot '%s' on layer %d", sources.size(),
	     shot ? obs_source_get_name(shot) : "", target);

	if (shot) {
		activateShot(target, shot, false);
	}
}

void LayersPanel::selectShot(obs_source_t *scene, int layer)
{
	selectedScene = scene ? OBSGetWeakRef(scene) : nullptr;
	selectedLayer = std::clamp(layer, 1, kLayerCount);
	for (LayersPanel *panel : panels) {
		panel->refreshStates();
	}
}

/* ---- AutoLive ----------------------------------------------------------- */

bool LayersPanel::autoLive()
{
	if (!autoLiveLoaded) {
		autoLiveEnabled = config_get_bool(App()->GetUserConfig(), kConfigSection, "AutoLive");
		autoLiveLoaded = true;
	}
	return autoLiveEnabled;
}

void LayersPanel::setAutoLive(bool enabled)
{
	autoLiveEnabled = enabled;
	autoLiveLoaded = true;
	config_set_bool(App()->GetUserConfig(), kConfigSection, "AutoLive", enabled);

	for (LayersPanel *panel : panels) {
		emit panel->autoLiveChanged(enabled);
	}
}

void LayersPanel::goLiveIfAutoLive()
{
	if (autoLive() && obs_frontend_preview_program_mode_active()) {
		obs_frontend_preview_program_trigger_transition();
	}
}

void LayersPanel::takeShotsWithHotkey(int key)
{
	bool taken = false;
	for (const OBSSource &scene : getShotScenes()) {
		if (getShotHotkey(scene) == key) {
			activateShot(getShotLayer(scene), scene, false);
			selectShot(scene, getShotLayer(scene));
			taken = true;
		}
	}

	if (taken) {
		goLiveIfAutoLive();
	}
}

/* ---- Layer visibility and navigation ------------------------------------ */

bool LayersPanel::isLayerVisible(int layer)
{
	return (hiddenLayerMask() & (1 << layer)) == 0;
}

void LayersPanel::setLayerVisible(int layer, bool visible)
{
	OBSScene mix = findMixScene();
	if (!mix) {
		mix = createMixScene();
	}

	obs_source_t *mixSource = obs_scene_get_source(mix);
	OBSDataAutoRelease priv = obs_source_get_private_settings(mixSource);
	int mask = (int)obs_data_get_int(priv, kHiddenKey);
	mask = visible ? (mask & ~(1 << layer)) : (mask | (1 << layer));
	obs_data_set_int(priv, kHiddenKey, mask);

	for (const OBSSceneItem &item : getSceneItems(mix)) {
		if (getItemLayer(item) == layer) {
			obs_sceneitem_set_visible(item, visible);
		}
	}

	for (LayersPanel *panel : panels) {
		panel->refreshStates();
	}
}

void LayersPanel::goToLayer(int layer)
{
	selectedLayer = std::clamp(layer, 1, kLayerCount);

	if (LayerRow *row = rowFor(selectedLayer); row && row->isVisible()) {
		scroll->ensureWidgetVisible(row, 0, 0);
	}

	for (LayersPanel *panel : panels) {
		panel->refreshStates();
	}
}

/* ---- Zoom --------------------------------------------------------------- */

void LayersPanel::loadZooms()
{
	if (onlyLayer > 0) {
		return;
	}

	const char *saved = config_get_string(App()->GetUserConfig(), kConfigSection, "LayerZoom");
	const QStringList parts = QString::fromUtf8(saved ? saved : "").split(',', Qt::SkipEmptyParts);
	for (int i = 0; i < kLayerCount && i < parts.size(); i++) {
		zooms[i] = std::clamp(parts[i].toInt(), kMinZoom, kMaxZoom);
	}
}

void LayersPanel::saveZooms()
{
	if (onlyLayer > 0) {
		return;
	}

	QStringList parts;
	for (int zoom : zooms) {
		parts << QString::number(zoom);
	}
	config_set_string(App()->GetUserConfig(), kConfigSection, "LayerZoom", QT_TO_UTF8(parts.join(',')));
}

int LayersPanel::layerZoom(int layer) const
{
	return (layer >= 1 && layer <= kLayerCount) ? zooms[layer - 1] : 100;
}

void LayersPanel::setLayerZoom(int layer, int percent)
{
	if (layer < 1 || layer > kLayerCount) {
		return;
	}

	percent = std::clamp(percent, kMinZoom, kMaxZoom);
	if (zooms[layer - 1] == percent) {
		return;
	}

	zooms[layer - 1] = percent;
	for (ShotTile *tile : tiles) {
		if (tile->getLayer() == layer) {
			tile->setZoom(percent);
		}
	}
	rows[layer - 1]->setZoom(percent);
	saveZooms();
}

void LayersPanel::setAllLayersZoom(int percent)
{
	for (int layer = 1; layer <= kLayerCount; layer++) {
		setLayerZoom(layer, percent);
	}
}

void LayersPanel::zoomAllLayers(int stepPercent)
{
	for (int layer = 1; layer <= kLayerCount; layer++) {
		setLayerZoom(layer, zooms[layer - 1] + stepPercent);
	}
}

/* ---- OBS events --------------------------------------------------------- */

void LayersPanel::obsSceneListChanged(void *data, calldata_t *params)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	if (!isRegularScene(source)) {
		return;
	}

	LayersPanel *panel = static_cast<LayersPanel *>(data);
	QMetaObject::invokeMethod(panel, &LayersPanel::scheduleRebuild, Qt::QueuedConnection);
}

void LayersPanel::obsSceneActiveChanged(void *data, calldata_t *params)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	if (!isRegularScene(source)) {
		return;
	}

	LayersPanel *panel = static_cast<LayersPanel *>(data);
	QMetaObject::invokeMethod(panel, &LayersPanel::refreshStates, Qt::QueuedConnection);
}

void LayersPanel::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	LayersPanel *panel = static_cast<LayersPanel *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		panel->collectionLoading = true;
		panel->rebuildTimer.stop();
		panel->clearAllTiles();
		panel->weakMixScene = nullptr;
		selectedScene = nullptr;
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		panel->collectionLoading = true;
		panel->rebuildTimer.stop();
		panel->clearAllTiles();
		panel->weakMixScene = nullptr;
		selectedScene = nullptr;
		obs_frontend_remove_event_callback(&LayersPanel::onFrontendEvent, panel);
		panel->eventCallbackRegistered = false;
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		panel->collectionLoading = false;
		panel->scheduleRebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		panel->scheduleRebuild();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		panel->refreshStates();
		break;
	default:
		break;
	}
}

void LayersPanel::scheduleRebuild()
{
	if (!collectionLoading) {
		rebuildTimer.start();
	}
}

void LayersPanel::clearAllTiles()
{
	for (ShotTile *tile : tiles) {
		tile->hide();
		tile->deleteLater();
	}
	tiles.clear();
	nextThumbnail = 0;
}

void LayersPanel::rebuild()
{
	if (collectionLoading) {
		return;
	}

	clearAllTiles();

	auto addTile = [this](obs_source_t *scene, int layer) {
		ShotTile *tile = new ShotTile(rows[layer - 1], scene, layer, zooms[layer - 1]);
		rows[layer - 1]->addTile(tile);
		tiles.push_back(tile);

		if (scene) {
			tile->setHotkey(getShotHotkey(scene));
			tile->setLockedIcon(decodeIcon(scene));
		}

		connect(tile, &ShotTile::clicked, this, [this, tile]() {
			if (tile->isClearTile()) {
				selectShot(nullptr, tile->getLayer());
				activateShot(tile->getLayer(), nullptr);
			} else if (OBSSource scene = tile->getScene()) {
				selectShot(scene, tile->getLayer());
				activateShot(tile->getLayer(), scene);
			}
		});
		connect(tile, &ShotTile::doubleClicked, this, [this, tile]() {
			if (OBSSource scene = tile->getScene()) {
				selectShot(scene, tile->getLayer());
				editShot(scene);
			}
		});
		connect(tile, &ShotTile::menuRequested, this,
			[this, tile](const QPoint &pos) { showTileMenu(tile, pos); });
	};

	for (int layer = 1; layer <= kLayerCount; layer++) {
		if (showsLayer(layer)) {
			addTile(nullptr, layer);
		}
	}

	for (const OBSSource &scene : getShotScenes()) {
		const int layer = getShotLayer(scene);
		if (showsLayer(layer)) {
			addTile(scene, layer);
		}
	}

	refreshStates();
}

void LayersPanel::refreshStates()
{
	OBSScene mix = findMixScene();

	std::vector<obs_source_t *> activeShots(kLayerCount + 1, nullptr);
	if (mix) {
		for (const OBSSceneItem &item : getSceneItems(mix)) {
			int layer = getItemLayer(item);
			if (layer >= 1 && layer <= kLayerCount) {
				activeShots[layer] = obs_sceneitem_get_source(item);
			}
		}
	}

	const int hidden = hiddenLayerMask();
	for (int layer = 1; layer <= kLayerCount; layer++) {
		rows[layer - 1]->setState((hidden & (1 << layer)) != 0, layer == selectedLayer);
	}

	for (ShotTile *tile : tiles) {
		obs_source_t *activeShot = activeShots[tile->getLayer()];
		tile->setLayerHidden((hidden & (1 << tile->getLayer())) != 0);

		/* Wirecast only highlights the Clear Layer of the layer you clicked */
		if (tile->isClearTile()) {
			tile->setActive(activeShot == nullptr && tile->getLayer() == selectedLayer);
			continue;
		}

		OBSSource scene = tile->getScene();
		tile->setActive(scene && scene.Get() == activeShot);
		tile->setLive(scene && obs_source_active(scene));
	}

	emit shotSelectionChanged();
}

void LayersPanel::updateNextThumbnail()
{
	if (tiles.empty()) {
		return;
	}

	/* Clear tiles have no thumbnail, so step over them */
	for (size_t i = 0; i < tiles.size(); i++) {
		ShotTile *tile = tiles[nextThumbnail++ % tiles.size()];
		if (!tile->isClearTile()) {
			tile->requestThumbnailUpdate();
			return;
		}
	}
}

/* ---- Layer Mix ---------------------------------------------------------- */

OBSScene LayersPanel::findMixScene()
{
	OBSSource cached = OBSGetStrongRef(weakMixScene);
	if (cached && !obs_source_removed(cached) && isMixScene(cached)) {
		return obs_scene_from_source(cached);
	}

	weakMixScene = nullptr;

	if (OBSSource mix = findMixSceneSource()) {
		weakMixScene = OBSGetWeakRef(mix);
		return obs_scene_from_source(mix);
	}

	return nullptr;
}

OBSScene LayersPanel::createMixScene()
{
	const std::string name = QT_TO_UTF8(uniqueSourceName(QTStr("Basic.Layers.MixName")));

	OBSSceneAutoRelease scene = obs_scene_create(name.c_str());
	obs_source_t *source = obs_scene_get_source(scene);

	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	obs_data_set_bool(priv, kMixKey, true);

	weakMixScene = OBSGetWeakRef(source);
	return scene.Get();
}

void LayersPanel::showMixScene(obs_scene_t *mix)
{
	obs_source_t *mixSource = obs_scene_get_source(mix);

	if (obs_frontend_preview_program_mode_active()) {
		OBSSourceAutoRelease current = obs_frontend_get_current_preview_scene();
		if (current.Get() != mixSource) {
			obs_frontend_set_current_preview_scene(mixSource);
		}
	} else {
		OBSSourceAutoRelease current = obs_frontend_get_current_scene();
		if (current.Get() != mixSource) {
			obs_frontend_set_current_scene(mixSource);
		}
	}
}

void LayersPanel::activateShot(int layer, obs_source_t *scene, bool allowAutoLive)
{
	OBSScene mix = findMixScene();
	if (!mix) {
		if (!scene) {
			return;
		}
		mix = createMixScene();
	}

	bool alreadyActive = false;
	for (const OBSSceneItem &item : getSceneItems(mix)) {
		if (getItemLayer(item) != layer) {
			continue;
		}

		if (scene && obs_sceneitem_get_source(item) == scene) {
			alreadyActive = true;
		} else {
			obs_sceneitem_remove(item);
		}
	}

	if (scene && !alreadyActive) {
		obs_sceneitem_t *item = obs_scene_add(mix, scene);
		if (!item) {
			blog(LOG_WARNING, "Layers: could not add shot '%s' to layer %d", obs_source_get_name(scene),
			     layer);
		} else {
			OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
			obs_data_set_int(priv, kLayerKey, layer);

			/* A hidden layer keeps its new shot hidden (Switch > Layer Visibility) */
			if (!isLayerVisible(layer)) {
				obs_sceneitem_set_visible(item, false);
			}

			/* Don't leave the new shot selected with transform handles in Preview */
			obs_sceneitem_select(item, false);
		}
	}

	reorderMixScene(mix);
	showMixScene(mix);

	for (LayersPanel *panel : panels) {
		panel->refreshStates();
	}

	if (allowAutoLive) {
		goLiveIfAutoLive();
	}
}

void LayersPanel::setShotLayer(obs_source_t *scene, int layer)
{
	if (getShotLayer(scene) == layer) {
		return;
	}

	/* A shot that changes layer leaves the mix, like dragging a live shot in Wirecast */
	if (OBSScene mix = findMixScene()) {
		for (const OBSSceneItem &item : getSceneItems(mix)) {
			if (getItemLayer(item) > 0 && obs_sceneitem_get_source(item) == scene) {
				obs_sceneitem_remove(item);
			}
		}
	}

	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	obs_data_set_int(priv, kLayerKey, layer);

	if (selectedShot().Get() == scene) {
		selectedLayer = layer;
	}

	refreshAll();
}

/* ---- Shot commands ------------------------------------------------------ */

void LayersPanel::editShot(obs_source_t *scene)
{
	/* Editing a shot must never put it on air, so do it in Studio Mode's Preview */
	if (!obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_preview_program_mode(true);
	}

	obs_frontend_set_current_preview_scene(scene);
}

void LayersPanel::renameShot(obs_source_t *scene)
{
	const QString current = QT_UTF8(obs_source_get_name(scene));

	for (;;) {
		std::string name;
		if (!NameDialog::AskForName(window(), QTStr("Wirecast.Shot.Rename.Title"),
					    QTStr("Wirecast.Shot.Rename.Text"), name, current)) {
			return;
		}

		if (name.empty()) {
			OBSMessageBox::warning(window(), QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}

		if (name == QT_TO_UTF8(current)) {
			return;
		}

		OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
		if (existing) {
			OBSMessageBox::warning(window(), QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}

		obs_source_set_name(scene, name.c_str());
		return;
	}
}

void LayersPanel::duplicateShot(obs_source_t *scene)
{
	obs_scene_t *original = obs_scene_from_source(scene);
	if (!original) {
		return;
	}

	const std::string name = QT_TO_UTF8(uniqueSourceName(QT_UTF8(obs_source_get_name(scene))));
	OBSSceneAutoRelease copy = obs_scene_duplicate(original, name.c_str(), OBS_SCENE_DUP_REFS);
	if (!copy) {
		return;
	}

	obs_source_t *copySource = obs_scene_get_source(copy);
	const int layer = getShotLayer(scene);

	OBSDataAutoRelease priv = obs_source_get_private_settings(copySource);
	obs_data_set_int(priv, kLayerKey, layer);
	obs_data_set_int(priv, kHotkeyKey, 0);
	obs_data_set_string(priv, kIconKey, "");
	obs_data_set_bool(priv, kMixKey, false);
	obs_data_set_bool(priv, kReplayKey, false);

	/* Duplicate Shot Options: these sources get their own new copy instead of being shared */
	const QStringList copyNames = getDupCopyNames(scene);
	if (!copyNames.isEmpty()) {
		for (const OBSSceneItem &item : getSceneItems(copy)) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const QString sourceName = QT_UTF8(obs_source_get_name(source));
			if (!copyNames.contains(sourceName) || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) {
				continue;
			}

			const std::string newName = QT_TO_UTF8(uniqueSourceName(sourceName));
			OBSSourceAutoRelease newSource = obs_source_duplicate(source, newName.c_str(), false);
			if (newSource) {
				replaceItemSource(copy, item, newSource);
			}
		}
	}

	selectShot(copySource, layer);
	refreshAll();
}

void LayersPanel::deleteShot(obs_source_t *scene)
{
	const QString name = QT_UTF8(obs_source_get_name(scene));
	if (OBSMessageBox::question(window(), QTStr("ConfirmRemove.Title"), QTStr("ConfirmRemove.Text").arg(name)) !=
	    QMessageBox::Yes) {
		return;
	}

	if (selectedShot().Get() == scene) {
		selectedScene = nullptr;
	}
	obs_source_remove(scene);
}

void LayersPanel::setShotHotkey(obs_source_t *scene, int key)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
	obs_data_set_int(priv, kHotkeyKey, key);
	refreshAll();
}

void LayersPanel::unmapAllHotkeys()
{
	for (const OBSSource &scene : getShotScenes()) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
		obs_data_set_int(priv, kHotkeyKey, 0);
	}
	refreshAll();
}

void LayersPanel::lockShotIcon(obs_source_t *scene, ShotTile *tile, bool lock)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(scene);

	if (lock && tile && !tile->currentPixmap().isNull()) {
		QByteArray png;
		QBuffer buffer(&png);
		buffer.open(QIODevice::WriteOnly);
		tile->currentPixmap().save(&buffer, "PNG");
		obs_data_set_string(priv, kIconKey, png.toBase64().constData());
	} else {
		obs_data_set_string(priv, kIconKey, "");
	}

	refreshAll();
}

void LayersPanel::changeShotSource(obs_source_t *scene, obs_source_t *source, bool audio)
{
	obs_scene_t *shot = obs_scene_from_source(scene);
	if (!shot) {
		return;
	}

	if (audio && !source) {
		for (const OBSSceneItem &item : getSceneItems(shot)) {
			if (isAudioOnly(obs_sceneitem_get_source(item))) {
				obs_sceneitem_remove(item);
			}
		}
		return;
	}

	if (obs_sceneitem_t *target = findShotItem(shot, audio)) {
		replaceItemSource(shot, target, source);
		return;
	}

	obs_sceneitem_t *item = obs_scene_add(shot, source);
	if (item && !audio) {
		fitItemToCanvas(item);
		obs_sceneitem_set_order_position(item, 0);
	}
	if (item) {
		obs_sceneitem_select(item, false);
	}
}

void LayersPanel::fillMoveToLayerMenu(QMenu *moveMenu, obs_source_t *scene)
{
	const int currentShotLayer = getShotLayer(scene);
	OBSSource shot = scene;

	for (int layer = 1; layer <= kLayerCount; layer++) {
		QAction *action = moveMenu->addAction(QTStr("Basic.Layers.Layer").arg(layer), this,
						      [this, shot, layer]() { setShotLayer(shot, layer); });
		action->setCheckable(true);
		action->setChecked(layer == currentShotLayer);
	}
}

void LayersPanel::fillDuplicateOptionsMenu(QMenu *options, obs_source_t *scene)
{
	options->setToolTipsVisible(true);
	OBSSource shot = scene;

	const QStringList copyNames = getDupCopyNames(scene);
	std::set<std::string> seen;
	for (const OBSSceneItem &item : getSceneItems(obs_scene_from_source(scene))) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) {
			continue;
		}

		const std::string sourceName = obs_source_get_name(source);
		if (!seen.insert(sourceName).second) {
			continue;
		}

		const QString name = QT_UTF8(sourceName.c_str());
		QAction *action = options->addAction(name);
		action->setCheckable(true);
		action->setChecked(copyNames.contains(name));
		action->setToolTip(QTStr("Wirecast.Shot.DuplicateOptions.Tooltip"));
		connect(action, &QAction::toggled, this, [shot, name](bool checked) {
			QStringList names = getDupCopyNames(shot);
			names.removeAll(name);
			if (checked) {
				names << name;
			}
			setDupCopyNames(shot, names);
		});
	}

	options->setEnabled(!options->isEmpty());
}

void LayersPanel::addShotMenuItems(QMenu *menu, obs_source_t *scene, ShotTile *tile)
{
	OBSSource shot = scene;
	obs_scene_t *shotScene = obs_scene_from_source(scene);

	/* Change Source / Change Audio */
	obs_sceneitem_t *videoItem = findShotItem(shotScene, false);
	obs_sceneitem_t *audioItem = findShotItem(shotScene, true);
	obs_source_t *currentVideo = videoItem ? obs_sceneitem_get_source(videoItem) : nullptr;
	obs_source_t *currentAudio = audioItem ? obs_sceneitem_get_source(audioItem) : nullptr;

	QMenu *sourceMenu = menu->addMenu(QTStr("Wirecast.Shot.ChangeSource"));
	for (const OBSSource &input : getInputs(false)) {
		QAction *action = sourceMenu->addAction(QT_UTF8(obs_source_get_name(input)), this,
							[this, shot, input]() { changeShotSource(shot, input, false); });
		action->setCheckable(true);
		action->setChecked(input.Get() == currentVideo);
	}
	sourceMenu->setEnabled(!sourceMenu->isEmpty());

	QMenu *audioMenu = menu->addMenu(QTStr("Wirecast.Shot.ChangeAudio"));
	QAction *noAudio = audioMenu->addAction(QTStr("None"), this,
						[this, shot]() { changeShotSource(shot, nullptr, true); });
	noAudio->setCheckable(true);
	noAudio->setChecked(!currentAudio);
	audioMenu->addSeparator();
	for (const OBSSource &input : getInputs(true)) {
		QAction *action = audioMenu->addAction(QT_UTF8(obs_source_get_name(input)), this,
						       [this, shot, input]() { changeShotSource(shot, input, true); });
		action->setCheckable(true);
		action->setChecked(input.Get() == currentAudio);
	}

	/* Keyboard Shortcut: press the number to take every shot mapped to it */
	QMenu *keyMenu = menu->addMenu(QTStr("Wirecast.Shot.KeyboardShortcut"));
	const int currentKey = getShotHotkey(scene);
	for (int key = 1; key <= 10; key++) {
		QAction *action = keyMenu->addAction(QString::number(key % 10), this, [this, shot, key, currentKey]() {
			setShotHotkey(shot, key == currentKey ? 0 : key);
		});
		action->setCheckable(true);
		action->setChecked(key == currentKey);
	}
	keyMenu->addSeparator();
	keyMenu->addAction(QTStr("Wirecast.Shot.UnmapAllKeys"), this, [this]() { unmapAllHotkeys(); });

	menu->addSeparator();
	menu->addAction(withShortcut(QTStr("Basic.Layers.EditShot"), "Ctrl+E"), this,
			[this, shot]() { editShot(shot); });
	menu->addAction(withShortcut(QTStr("Wirecast.Shot.Rename"), "Ctrl+R"), this,
			[this, shot]() { renameShot(shot); });
	menu->addAction(withShortcut(QTStr("Wirecast.Shot.Duplicate"), "Ctrl+D"), this,
			[this, shot]() { duplicateShot(shot); });
	fillDuplicateOptionsMenu(menu->addMenu(QTStr("Wirecast.Shot.DuplicateOptions")), scene);
	menu->addAction(withShortcut(QTStr("Wirecast.Shot.Delete"), "Ctrl+Backspace"), this,
			[this, shot]() { deleteShot(shot); });

	menu->addSeparator();
	fillMoveToLayerMenu(menu->addMenu(QTStr("Basic.Layers.MoveToLayer")), scene);

	menu->addSeparator();
	const bool locked = !decodeIcon(scene).isNull();
	QPointer<ShotTile> tilePtr = tile;
	menu->addAction(locked ? QTStr("Wirecast.Shot.UnlockIcon") : QTStr("Wirecast.Shot.LockIcon"), this,
			[this, shot, tilePtr, locked]() { lockShotIcon(shot, tilePtr, !locked); });
	QAction *refresh = menu->addAction(QTStr("Wirecast.Shot.RefreshIcon"), this, [tilePtr]() {
		if (tilePtr) {
			tilePtr->requestThumbnailUpdate();
		}
	});
	refresh->setEnabled(tile && !locked);
}

/* ---- New shots ---------------------------------------------------------- */

OBSSource LayersPanel::createShot(const QString &baseName, const std::vector<OBSSource> &sources,
				  const std::function<void(obs_sceneitem_t *item, size_t index)> &setupItem)
{
	const std::string name = QT_TO_UTF8(uniqueSourceName(baseName));
	OBSSceneAutoRelease scene = obs_scene_create(name.c_str());
	obs_source_t *sceneSource = obs_scene_get_source(scene);

	OBSDataAutoRelease priv = obs_source_get_private_settings(sceneSource);
	obs_data_set_int(priv, kLayerKey, selectedLayer);

	for (size_t i = 0; i < sources.size(); i++) {
		obs_sceneitem_t *item = obs_scene_add(scene, sources[i]);
		if (!item) {
			continue;
		}
		if (setupItem) {
			setupItem(item, i);
		} else if (hasVideo(sources[i])) {
			fitItemToCanvas(item);
		}
		obs_sceneitem_select(item, false);
	}

	selectShot(sceneSource, selectedLayer);
	refreshAll();
	return OBSSource(sceneSource);
}

void LayersPanel::importMedia(const QStringList &files)
{
	static const QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "gif", "tga", "webp", "psd", "jxr"};

	for (const QString &file : files) {
		const QFileInfo info(file);
		const bool image = imageExtensions.contains(info.suffix().toLower());

		OBSDataAutoRelease settings = obs_data_create();
		if (image) {
			obs_data_set_string(settings, "file", QT_TO_UTF8(file));
		} else {
			obs_data_set_bool(settings, "is_local_file", true);
			obs_data_set_string(settings, "local_file", QT_TO_UTF8(file));
		}

		const std::string sourceName = QT_TO_UTF8(uniqueSourceName(info.completeBaseName()));
		OBSSourceAutoRelease source =
			obs_source_create(image ? "image_source" : "ffmpeg_source", sourceName.c_str(), settings, nullptr);
		if (!source) {
			blog(LOG_WARNING, "Layers: could not import '%s'", QT_TO_UTF8(file));
			continue;
		}

		createShot(info.completeBaseName(), {OBSSource(source.Get())});
	}
}

void LayersPanel::createReplayShot(const QString &file, bool updateExisting)
{
	if (file.isEmpty()) {
		return;
	}

	if (updateExisting) {
		for (const OBSSource &scene : getShotScenes()) {
			OBSDataAutoRelease priv = obs_source_get_private_settings(scene);
			if (!obs_data_get_bool(priv, kReplayKey)) {
				continue;
			}

			for (const OBSSceneItem &item : getSceneItems(obs_scene_from_source(scene))) {
				obs_source_t *source = obs_sceneitem_get_source(item);
				if (strcmp(obs_source_get_id(source), "ffmpeg_source") != 0) {
					continue;
				}
				OBSDataAutoRelease settings = obs_data_create();
				obs_data_set_string(settings, "local_file", QT_TO_UTF8(file));
				obs_source_update(source, settings);
				obs_source_media_restart(source);
				return;
			}
		}
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", QT_TO_UTF8(file));
	obs_data_set_bool(settings, "restart_on_activate", true);

	const QString baseName = QTStr("Wirecast.Replay.ShotName");
	const std::string sourceName = QT_TO_UTF8(uniqueSourceName(baseName));
	OBSSourceAutoRelease source = obs_source_create("ffmpeg_source", sourceName.c_str(), settings, nullptr);
	if (!source) {
		return;
	}

	OBSSource shot = createShot(baseName, {OBSSource(source.Get())});
	OBSDataAutoRelease priv = obs_source_get_private_settings(shot);
	obs_data_set_bool(priv, kReplayKey, true);
}

/* ---- Menus -------------------------------------------------------------- */

void LayersPanel::showTileMenu(ShotTile *tile, const QPoint &globalPos)
{
	if (tile->isClearTile()) {
		showRowMenu(tile->getLayer(), globalPos);
		return;
	}

	OBSSource scene = tile->getScene();
	if (!scene) {
		return;
	}

	selectShot(scene, tile->getLayer());

	QMenu menu(this);
	addShotMenuItems(&menu, scene, tile);
	menu.exec(globalPos);
}

void LayersPanel::showRowMenu(int layer, const QPoint &globalPos)
{
	goToLayer(layer);

	QMenu menu(this);

	QMenu *addMenu = menu.addMenu(QTStr("Basic.Layers.AddShot"));
	for (const OBSSource &scene : getShotScenes()) {
		if (getShotLayer(scene) == layer) {
			continue;
		}

		addMenu->addAction(QT_UTF8(obs_source_get_name(scene)), this,
				   [this, scene, layer]() { setShotLayer(scene, layer); });
	}
	addMenu->setEnabled(!addMenu->isEmpty());

	menu.addAction(QTStr("Wirecast.File.ImportMedia"), this, [this]() {
		const QStringList files = QFileDialog::getOpenFileNames(window(), QTStr("Wirecast.File.ImportMedia"));
		importMedia(files);
	});

	menu.addSeparator();
	const bool visible = isLayerVisible(layer);
	QAction *visibleAction = menu.addAction(QTStr("Wirecast.Layers.Visible"), this,
						[this, layer, visible]() { setLayerVisible(layer, !visible); });
	visibleAction->setCheckable(true);
	visibleAction->setChecked(visible);
	menu.addAction(QTStr("Basic.Layers.ClearLayer"), this, [this, layer]() { activateShot(layer, nullptr); });

	menu.exec(globalPos);
}

void LayersPanel::showEvent(QShowEvent *event)
{
	QFrame::showEvent(event);
	thumbnailTimer.start();
}

void LayersPanel::hideEvent(QHideEvent *event)
{
	QFrame::hideEvent(event);
	thumbnailTimer.stop();
}
