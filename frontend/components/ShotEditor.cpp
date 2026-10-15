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

#include "ShotEditor.hpp"
#include "LayersPanel.hpp"
#include "MediaControls.hpp"
#include "VolumeMeter.hpp"
#include "WirecastIcons.hpp"

#include <OBSApp.hpp>
#include <dialogs/NameDialog.hpp>
#include <widgets/OBSBasic.hpp>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "moc_ShotEditor.cpp"

using WirecastIcons::Glyph;

namespace {
constexpr const char *kColorFilterName = "Wirecast Color";
constexpr const char *kColorFilterId = "color_filter_v2";
constexpr const char *kKeyFilterName = "Wirecast Key";
constexpr const char *kKeyFilterIds[] = {"chroma_key_filter_v2", "luma_key_filter_v2", "color_key_filter_v2"};
constexpr const char *kKeyColorTypes[] = {"green", "blue", "magenta", "custom"};

constexpr int kRowIconSize = 16;
constexpr int kToolIconSize = 14;
constexpr int kLabelWidth = 96;

/* ---- OBS helpers -------------------------------------------------------- */

QSize canvasSize()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		return QSize(int(ovi.base_width), int(ovi.base_height));
	}
	return QSize(1920, 1080);
}

/* Bottom of the stack first, as libobs stores them */
std::vector<OBSSceneItem> sceneItems(obs_scene_t *scene)
{
	std::vector<OBSSceneItem> items;
	if (!scene) {
		return items;
	}
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		},
		&items);
	return items;
}

bool hasAudio(obs_source_t *source)
{
	return source && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
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

QIcon sourceIcon(obs_source_t *source)
{
	OBSBasic *main = OBSBasic::Get();
	if (!main || !source) {
		return QIcon();
	}
	if (obs_source_is_scene(source)) {
		return main->GetSceneIcon();
	}
	return main->GetSourceIcon(obs_source_get_id(source));
}

QIcon twoStateIcon(Glyph off, Glyph on, int size)
{
	const qreal ratio = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
	QIcon icon;
	icon.addPixmap(WirecastIcons::pixmap(off, WirecastIcons::kIconMuted, size, ratio), QIcon::Normal, QIcon::Off);
	icon.addPixmap(WirecastIcons::pixmap(on, WirecastIcons::kIconColor, size, ratio), QIcon::Normal, QIcon::On);
	return icon;
}

/* Centre of the layer's bounding box in canvas pixels */
QPointF itemCenter(obs_sceneitem_t *item)
{
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	vec3 p;
	vec3_set(&p, 0.5f, 0.5f, 0.0f);
	vec3_transform(&p, &p, &box);
	return QPointF(p.x, p.y);
}

QSizeF itemSize(obs_sceneitem_t *item, const obs_transform_info &info)
{
	if (info.bounds_type != OBS_BOUNDS_NONE) {
		return QSizeF(info.bounds.x, info.bounds.y);
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const double cx = std::max(0, int(obs_source_get_width(source)) - crop.left - crop.right);
	const double cy = std::max(0, int(obs_source_get_height(source)) - crop.top - crop.bottom);
	return QSizeF(cx * std::fabs(info.scale.x), cy * std::fabs(info.scale.y));
}

/* Transform info re-anchored on the layer's centre, so position, size and
 * rotation all work around the middle of the layer as in Wirecast */
obs_transform_info centeredInfo(obs_sceneitem_t *item)
{
	const QPointF center = itemCenter(item);
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	info.alignment = OBS_ALIGN_CENTER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.pos.x = float(center.x());
	info.pos.y = float(center.y());
	return info;
}

/* Wirecast Fill modes and the OBS bounding box types that match them */
constexpr obs_bounds_type kFillTypes[] = {OBS_BOUNDS_SCALE_INNER, OBS_BOUNDS_SCALE_TO_WIDTH,
					  OBS_BOUNDS_SCALE_TO_HEIGHT, OBS_BOUNDS_STRETCH};

int fillIndex(obs_bounds_type type)
{
	for (int i = 0; i < int(std::size(kFillTypes)); i++) {
		if (kFillTypes[i] == type) {
			return i;
		}
	}
	return 0;
}

OBSSource findFilter(obs_source_t *source, const char *name)
{
	OBSSourceAutoRelease filter = obs_source_get_filter_by_name(source, name);
	return filter.Get();
}

/* The layer's key filter: ours, or a chroma/luma/color key the user added */
OBSSource findKeyFilter(obs_source_t *source)
{
	if (OBSSource filter = findFilter(source, kKeyFilterName)) {
		return filter;
	}

	OBSSource found;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			OBSSource &result = *static_cast<OBSSource *>(param);
			if (result) {
				return;
			}
			const char *id = obs_source_get_id(filter);
			for (const char *keyId : kKeyFilterIds) {
				if (strcmp(id, keyId) == 0) {
					result = filter;
					return;
				}
			}
		},
		&found);
	return found;
}

int keyTypeIndex(obs_source_t *filter)
{
	if (filter) {
		const char *id = obs_source_get_id(filter);
		for (int i = 0; i < int(std::size(kKeyFilterIds)); i++) {
			if (strcmp(id, kKeyFilterIds[i]) == 0) {
				return i;
			}
		}
	}
	return 0;
}

OBSSource addFilter(obs_source_t *source, const char *id, const char *name)
{
	OBSSourceAutoRelease filter = obs_source_create(id, name, nullptr, nullptr);
	if (filter) {
		obs_source_filter_add(source, filter);
	}
	return filter.Get();
}

void updateFilter(obs_source_t *filter, const std::function<void(obs_data_t *)> &set)
{
	OBSDataAutoRelease settings = obs_data_create();
	set(settings);
	obs_source_update(filter, settings);
}

/* Wirecast's centred empty state: a large title with a line of help under it */
QString emptyText(const char *key)
{
	return QString("<div style=\"font-size:15pt; color:#C9CAD1;\">%1</div>"
		       "<div style=\"margin-top:4px;\">%2</div>")
		.arg(QTStr(key).toHtmlEscaped(), QTStr((std::string(key) + ".Text").c_str()).toHtmlEscaped());
}

void setupEmptyLabel(QLabel *label)
{
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignCenter);
	label->setTextFormat(Qt::RichText);
	label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	setClasses(label, "wc-editor-empty");
}

/* ---- Small widgets ------------------------------------------------------ */

/* Label + slider + number, as in Wirecast's property rows */
class SliderRow : public QWidget {
	QSlider *slider;
	QDoubleSpinBox *spin;
	double scale;

public:
	std::function<void(double)> changed;

	SliderRow(QWidget *parent, double min, double max, int decimals, const QString &suffix = QString())
		: QWidget(parent),
		  scale(std::pow(10.0, decimals))
	{
		QHBoxLayout *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);

		slider = new QSlider(Qt::Horizontal, this);
		spin = new QDoubleSpinBox(this);
		spin->setDecimals(decimals);
		spin->setSuffix(suffix);
		spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
		spin->setFixedWidth(58);
		spin->setAlignment(Qt::AlignRight);
		setRange(min, max);

		layout->addWidget(slider, 1);
		layout->addWidget(spin);

		connect(slider, &QSlider::valueChanged, this, [this](int value) {
			const double v = value / scale;
			{
				QSignalBlocker block(spin);
				spin->setValue(v);
			}
			if (changed) {
				changed(v);
			}
		});
		connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
			{
				QSignalBlocker block(slider);
				slider->setValue(int(std::lround(v * scale)));
			}
			if (changed) {
				changed(v);
			}
		});
	}

	void setRange(double min, double max)
	{
		QSignalBlocker blockSlider(slider);
		QSignalBlocker blockSpin(spin);
		slider->setRange(int(std::lround(min * scale)), int(std::lround(max * scale)));
		spin->setRange(min, max);
	}

	void setSpecialValueText(const QString &text) { spin->setSpecialValueText(text); }

	void setValue(double v)
	{
		QSignalBlocker blockSlider(slider);
		slider->setValue(int(std::lround(v * scale)));
		if (!spin->hasFocus()) {
			QSignalBlocker blockSpin(spin);
			spin->setValue(v);
		}
	}
};

QLabel *rowLabel(const QString &text, QWidget *parent)
{
	QLabel *label = new QLabel(text, parent);
	label->setMinimumWidth(kLabelWidth);
	setClasses(label, "wc-editor-label");
	return label;
}

void addRow(QGridLayout *grid, const QString &label, QWidget *widget)
{
	const int row = grid->rowCount();
	grid->addWidget(rowLabel(label, widget->parentWidget()), row, 0);
	grid->addWidget(widget, row, 1);
}

QGridLayout *newGrid(QWidget *parent)
{
	QGridLayout *grid = new QGridLayout(parent);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(6);
	grid->setColumnStretch(1, 1);
	return grid;
}

/* A collapsible group with a triangle, like Wirecast's Crop and Color panes */
QGridLayout *addSection(QVBoxLayout *layout, QWidget *parent, const QString &title, bool expanded = false)
{
	QToolButton *header = new QToolButton(parent);
	header->setText(title);
	header->setCheckable(true);
	header->setChecked(expanded);
	header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setClasses(header, "wc-editor-section");

	QWidget *body = new QWidget(parent);
	QGridLayout *grid = newGrid(body);
	grid->setContentsMargins(14, 2, 0, 6);
	body->setVisible(expanded);

	QObject::connect(header, &QToolButton::toggled, body, [header, body](bool on) {
		body->setVisible(on);
		header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
	});

	layout->addWidget(header);
	layout->addWidget(body);
	return grid;
}

QToolButton *toolButton(Glyph glyph, const QString &tip, QWidget *parent)
{
	QToolButton *button = new QToolButton(parent);
	button->setIcon(WirecastIcons::icon(glyph, kToolIconSize));
	button->setIconSize(QSize(kToolIconSize, kToolIconSize));
	button->setToolTip(tip);
	setClasses(button, "wc-editor-tool");
	return button;
}

QDoubleSpinBox *numberBox(QWidget *parent)
{
	QDoubleSpinBox *spin = new QDoubleSpinBox(parent);
	spin->setRange(-100000.0, 100000.0);
	spin->setDecimals(0);
	spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
	spin->setAlignment(Qt::AlignRight);
	spin->setKeyboardTracking(false);
	return spin;
}

void setNumber(QDoubleSpinBox *spin, double value)
{
	if (!spin->hasFocus()) {
		QSignalBlocker block(spin);
		spin->setValue(value);
	}
}

/* ---- Page base ---------------------------------------------------------- */

/* Scrolling page with a message for when there is nothing to edit */
class EditorPage : public QScrollArea {
protected:
	ShotEditor *editor;
	QLabel *emptyLabel;
	QWidget *body;
	QVBoxLayout *bodyLayout;

	void setEmpty(const QString &text)
	{
		emptyLabel->setText(text);
		emptyLabel->setVisible(!text.isEmpty());
		body->setVisible(text.isEmpty());
	}

	/* Empty-state text when no layer is selected, else an empty string */
	QString noLayerText() const
	{
		if (!editor->shot()) {
			return emptyText("Wirecast.ShotEditor.NoShot");
		}
		if (!editor->item()) {
			return emptyText("Wirecast.ShotEditor.NoLayer");
		}
		return QString();
	}

public:
	EditorPage(ShotEditor *editor_, QWidget *parent) : QScrollArea(parent), editor(editor_)
	{
		setObjectName("wcEditorPage");
		setWidgetResizable(true);
		setFrameShape(QFrame::NoFrame);
		setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

		QWidget *content = new QWidget(this);
		content->setObjectName("wcEditorContent");
		QVBoxLayout *layout = new QVBoxLayout(content);
		layout->setContentsMargins(16, 4, 12, 12);
		layout->setSpacing(6);

		emptyLabel = new QLabel(content);
		setupEmptyLabel(emptyLabel);
		layout->addWidget(emptyLabel, 100);

		body = new QWidget(content);
		bodyLayout = new QVBoxLayout(body);
		bodyLayout->setContentsMargins(0, 0, 0, 0);
		bodyLayout->setSpacing(6);
		layout->addWidget(body);
		layout->addStretch();

		setWidget(content);
	}
};

/* ======================================================================== */
/* Shot Layers                                                              */

class LayerItemRow : public QFrame {
	OBSSceneItem item;
	QLabel *iconLabel;
	QToolButton *speaker = nullptr;
	QPixmap iconPixmap;
	bool selected;
	OBSSignal muteSignal;

	static void sourceMuted(void *data, calldata_t *)
	{
		LayerItemRow *row = static_cast<LayerItemRow *>(data);
		QMetaObject::invokeMethod(row, [row]() { row->updateMute(); }, Qt::QueuedConnection);
	}

	void showIcon(bool show) { iconLabel->setPixmap(show ? iconPixmap : QPixmap()); }

public:
	std::function<void()> clicked;
	std::function<void()> doubleClicked;
	std::function<void(const QPoint &)> menuRequested;

	LayerItemRow(QWidget *parent, obs_sceneitem_t *item_, bool selected_)
		: QFrame(parent),
		  item(item_),
		  selected(selected_)
	{
		setObjectName("shotLayerRow");
		setClasses(this, selected ? "selected" : "");

		obs_source_t *source = obs_sceneitem_get_source(item);

		QHBoxLayout *layout = new QHBoxLayout(this);
		layout->setContentsMargins(8, 3, 6, 3);
		layout->setSpacing(6);

		/* Source icons appear only on hover, as in Wirecast */
		iconLabel = new QLabel(this);
		iconLabel->setFixedSize(kRowIconSize, kRowIconSize);
		iconPixmap = sourceIcon(source).pixmap(kRowIconSize, kRowIconSize);
		showIcon(selected);

		QLabel *name = new QLabel(QT_UTF8(obs_source_get_name(source)), this);
		name->setObjectName("shotLayerName");
		name->setTextInteractionFlags(Qt::NoTextInteraction);

		QToolButton *eye = new QToolButton(this);
		eye->setCheckable(true);
		eye->setChecked(obs_sceneitem_visible(item));
		eye->setIcon(twoStateIcon(Glyph::Eye, Glyph::Eye, kRowIconSize));
		eye->setIconSize(QSize(kRowIconSize, kRowIconSize));
		eye->setToolTip(QTStr("Wirecast.ShotEditor.LayerVisible"));
		setClasses(eye, "wc-layer-toggle");
		connect(eye, &QToolButton::toggled, this,
			[this](bool visible) { obs_sceneitem_set_visible(item, visible); });

		layout->addWidget(iconLabel);
		layout->addWidget(name, 1);
		layout->addWidget(eye);

		if (hasAudio(source)) {
			speaker = new QToolButton(this);
			speaker->setCheckable(true);
			speaker->setIcon(twoStateIcon(Glyph::OutputMuted, Glyph::OutputOn, kRowIconSize));
			speaker->setIconSize(QSize(kRowIconSize, kRowIconSize));
			speaker->setToolTip(QTStr("Wirecast.ShotEditor.LayerMute"));
			setClasses(speaker, "wc-layer-toggle");
			connect(speaker, &QToolButton::toggled, this, [this](bool on) {
				obs_source_set_muted(obs_sceneitem_get_source(item), !on);
			});
			muteSignal.Connect(obs_source_get_signal_handler(source), "mute", &LayerItemRow::sourceMuted,
					   this);
			updateMute();
			layout->addWidget(speaker);
		} else {
			/* Keep the eye icons in one column */
			layout->addSpacing(kRowIconSize + 8);
		}
	}

	void updateMute()
	{
		if (speaker) {
			QSignalBlocker block(speaker);
			speaker->setChecked(!obs_source_muted(obs_sceneitem_get_source(item)));
		}
	}

protected:
	void enterEvent(QEnterEvent *) override { showIcon(true); }
	void leaveEvent(QEvent *) override { showIcon(selected); }

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && clicked) {
			clicked();
		}
		QFrame::mousePressEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && doubleClicked) {
			doubleClicked();
		}
	}

	void contextMenuEvent(QContextMenuEvent *event) override
	{
		if (clicked) {
			clicked();
		}
		if (menuRequested) {
			menuRequested(event->globalPos());
		}
	}
};

class ShotLayersPage : public QWidget {
	ShotEditor *editor;
	QLineEdit *nameEdit;
	QToolButton *addButton;
	QToolButton *removeButton;
	QToolButton *upButton;
	QToolButton *downButton;
	QToolButton *manageButton;
	QWidget *list;
	QVBoxLayout *listLayout;
	QLabel *emptyLabel;

	void updateName()
	{
		if (nameEdit->hasFocus()) {
			return;
		}
		OBSSource shot = editor->shot();
		nameEdit->setText(shot ? QT_UTF8(obs_source_get_name(shot)) : QString());
		nameEdit->setEnabled(!!shot);
	}

	void renameShot()
	{
		OBSSource shot = editor->shot();
		if (!shot) {
			return;
		}

		const std::string name = QT_TO_UTF8(nameEdit->text().trimmed());
		if (!name.empty() && name != obs_source_get_name(shot)) {
			OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
			if (!existing) {
				obs_source_set_name(shot, name.c_str());
			}
		}
		nameEdit->clearFocus();
		updateName();
	}

	void rebuild()
	{
		while (QLayoutItem *child = listLayout->takeAt(0)) {
			/* Later: this can run from inside a row's own click handler */
			if (QWidget *w = child->widget()) {
				w->hide();
				w->deleteLater();
			}
			delete child;
		}

		OBSScene scene = editor->shotScene();
		OBSSceneItem selected = editor->item();
		std::vector<OBSSceneItem> items = sceneItems(scene);

		/* Top of the list is the top of the stack */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_sceneitem_t *item = *it;
			LayerItemRow *row = new LayerItemRow(list, item, item == selected.Get());
			OBSSceneItem ref = item;
			row->clicked = [this, ref]() { editor->selectItem(ref); };
			row->doubleClicked = [ref]() {
				if (OBSBasic *main = OBSBasic::Get()) {
					main->CreatePropertiesWindow(obs_sceneitem_get_source(ref));
				}
			};
			row->menuRequested = [this](const QPoint &pos) { showManageMenu(pos); };
			listLayout->addWidget(row);
		}
		listLayout->addStretch();

		emptyLabel->setText(scene ? emptyText("Wirecast.ShotEditor.NoLayers")
					  : emptyText("Wirecast.ShotEditor.NoShot"));
		emptyLabel->setVisible(items.empty());

		const bool hasItem = !!selected;
		addButton->setEnabled(!!scene);
		removeButton->setEnabled(hasItem);
		manageButton->setEnabled(hasItem);
		upButton->setEnabled(hasItem && items.back().Get() != selected.Get());
		downButton->setEnabled(hasItem && items.front().Get() != selected.Get());
	}

	void addSource(obs_source_t *source)
	{
		OBSScene scene = editor->shotScene();
		if (!scene || !source) {
			return;
		}

		obs_sceneitem_t *item = obs_scene_add(scene, source);
		if (!item) {
			return;
		}

		/* New layers fill the canvas keeping their shape (Scale to Fit) */
		const QSize canvas = canvasSize();
		obs_transform_info info;
		obs_sceneitem_get_info2(item, &info);
		info.alignment = OBS_ALIGN_CENTER;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		info.bounds.x = float(canvas.width());
		info.bounds.y = float(canvas.height());
		info.pos.x = float(canvas.width()) / 2.0f;
		info.pos.y = float(canvas.height()) / 2.0f;
		obs_sceneitem_set_info2(item, &info);

		editor->selectItem(item);
	}

	void showAddMenu()
	{
		QMenu menu(this);

		struct TypeEntry {
			QString name;
			std::string id;
		};
		std::vector<TypeEntry> types;
		const char *type;
		const char *unversioned;
		for (size_t i = 0; obs_enum_input_types2(i, &type, &unversioned); i++) {
			const uint32_t caps = obs_get_source_output_flags(type);
			if ((caps & (OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DEPRECATED)) != 0) {
				continue;
			}
			types.push_back({QT_UTF8(obs_source_get_display_name(type)), type});
		}
		std::sort(types.begin(), types.end(),
			  [](const TypeEntry &a, const TypeEntry &b) { return a.name.localeAwareCompare(b.name) < 0; });

		OBSBasic *main = OBSBasic::Get();
		for (const TypeEntry &entry : types) {
			QAction *action = menu.addAction(main ? main->GetSourceIcon(entry.id.c_str()) : QIcon(),
							 entry.name);
			const std::string id = entry.id;
			const QString base = entry.name;
			connect(action, &QAction::triggered, this, [this, id, base]() {
				OBSSourceAutoRelease source = obs_source_create(id.c_str(), QT_TO_UTF8(uniqueSourceName(base)),
										 nullptr, nullptr);
				if (!source) {
					return;
				}
				addSource(source);
				if (OBSBasic *main = OBSBasic::Get()) {
					main->CreatePropertiesWindow(source);
				}
			});
		}

		menu.addSeparator();
		QMenu *existing = menu.addMenu(QTStr("Wirecast.ShotEditor.AddExisting"));
		std::vector<OBSSource> inputs;
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
				return true;
			},
			&inputs);
		std::sort(inputs.begin(), inputs.end(), [](const OBSSource &a, const OBSSource &b) {
			return QString::localeAwareCompare(QT_UTF8(obs_source_get_name(a)),
							   QT_UTF8(obs_source_get_name(b))) < 0;
		});
		for (const OBSSource &input : inputs) {
			QAction *action = existing->addAction(sourceIcon(input), QT_UTF8(obs_source_get_name(input)));
			OBSSource ref = input;
			connect(action, &QAction::triggered, this, [this, ref]() { addSource(ref); });
		}
		existing->setEnabled(!inputs.empty());

		menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height())));
	}

	void removeLayer()
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}

		const QString name = QT_UTF8(obs_source_get_name(obs_sceneitem_get_source(item)));
		if (OBSMessageBox::question(this, QTStr("ConfirmRemove.Title"),
					    QTStr("ConfirmRemove.Text").arg(name)) != QMessageBox::Yes) {
			return;
		}
		obs_sceneitem_remove(item);
	}

	void moveLayer(obs_order_movement movement)
	{
		if (OBSSceneItem item = editor->item()) {
			obs_sceneitem_set_order(item, movement);
		}
	}

	void showManageMenu(const QPoint &globalPos)
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		OBSSource source = obs_sceneitem_get_source(item);

		QMenu menu(this);
		connect(menu.addAction(QTStr("Properties")), &QAction::triggered, this, [source]() {
			if (OBSBasic *main = OBSBasic::Get()) {
				main->CreatePropertiesWindow(source);
			}
		});
		connect(menu.addAction(QTStr("Filters")), &QAction::triggered, this, [source]() {
			if (OBSBasic *main = OBSBasic::Get()) {
				main->CreateFiltersWindow(source);
			}
		});
		connect(menu.addAction(QTStr("Rename")), &QAction::triggered, this, [this, source]() {
			std::string name;
			if (!NameDialog::AskForName(this, QTStr("Wirecast.ShotEditor.RenameSource"),
						    QTStr("Wirecast.ShotEditor.RenameSource.Text"), name,
						    QT_UTF8(obs_source_get_name(source)))) {
				return;
			}
			if (name.empty() || name == obs_source_get_name(source)) {
				return;
			}
			OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
			if (existing) {
				OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
				return;
			}
			obs_source_set_name(source, name.c_str());
		});
		menu.addSeparator();
		connect(menu.addAction(QTStr("Remove")), &QAction::triggered, this, &ShotLayersPage::removeLayer);
		menu.exec(globalPos);
	}

public:
	ShotLayersPage(ShotEditor *editor_, QWidget *parent) : QWidget(parent), editor(editor_)
	{
		setObjectName("wcShotLayersPage");

		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);

		/* Name of the shot being edited */
		QWidget *nameRow = new QWidget(this);
		nameRow->setObjectName("wirecastShotNameRow");
		QHBoxLayout *nameLayout = new QHBoxLayout(nameRow);
		nameLayout->setContentsMargins(16, 0, 8, 8);
		nameLayout->setSpacing(6);
		nameEdit = new QLineEdit(nameRow);
		nameEdit->setObjectName("wirecastShotName");
		connect(nameEdit, &QLineEdit::editingFinished, this, [this]() { renameShot(); });
		nameLayout->addWidget(new QLabel(QTStr("Wirecast.ShotName"), nameRow));
		nameLayout->addWidget(nameEdit, 1);
		layout->addWidget(nameRow);

		/* + − and ↑ ↓ button pairs, centred under the name as in Wirecast */
		QWidget *tools = new QWidget(this);
		tools->setObjectName("wcShotLayerTools");
		QHBoxLayout *toolLayout = new QHBoxLayout(tools);
		toolLayout->setContentsMargins(16, 0, 8, 8);
		toolLayout->setSpacing(0);

		addButton = toolButton(Glyph::Plus, QTStr("Wirecast.ShotEditor.AddLayer"), tools);
		removeButton = toolButton(Glyph::Minus, QTStr("Wirecast.ShotEditor.RemoveLayer"), tools);
		upButton = toolButton(Glyph::Up, QTStr("Wirecast.ShotEditor.MoveUp"), tools);
		downButton = toolButton(Glyph::Down, QTStr("Wirecast.ShotEditor.MoveDown"), tools);
		manageButton = toolButton(Glyph::Gear, QTStr("Wirecast.ShotEditor.ManageLayers"), tools);

		/* Joined button pairs with a rounded outline, as in Wirecast */
		auto group = [tools](std::initializer_list<QToolButton *> buttons) {
			QFrame *frame = new QFrame(tools);
			setClasses(frame, "wc-tool-group");
			QHBoxLayout *h = new QHBoxLayout(frame);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(0);
			int i = 0;
			for (QToolButton *button : buttons) {
				button->setParent(frame);
				button->setFixedSize(29, 23);
				if (++i < int(buttons.size())) {
					setClasses(button, "wc-editor-tool wc-tool-divided");
				}
				h->addWidget(button);
			}
			return frame;
		};

		toolLayout->addStretch();
		toolLayout->addWidget(group({addButton, removeButton}));
		toolLayout->addSpacing(14);
		toolLayout->addWidget(group({upButton, downButton}));
		toolLayout->addSpacing(14);
		toolLayout->addWidget(group({manageButton}));
		toolLayout->addStretch();
		layout->addWidget(tools);

		connect(addButton, &QToolButton::clicked, this, [this]() { showAddMenu(); });
		connect(removeButton, &QToolButton::clicked, this, [this]() { removeLayer(); });
		connect(upButton, &QToolButton::clicked, this, [this]() { moveLayer(OBS_ORDER_MOVE_UP); });
		connect(downButton, &QToolButton::clicked, this, [this]() { moveLayer(OBS_ORDER_MOVE_DOWN); });
		connect(manageButton, &QToolButton::clicked, this, [this]() {
			showManageMenu(manageButton->mapToGlobal(QPoint(0, manageButton->height())));
		});

		QScrollArea *scroll = new QScrollArea(this);
		scroll->setObjectName("wcShotLayerList");
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

		QWidget *content = new QWidget(scroll);
		content->setObjectName("wcShotLayerListContent");
		QVBoxLayout *contentLayout = new QVBoxLayout(content);
		contentLayout->setContentsMargins(0, 0, 0, 0);
		contentLayout->setSpacing(0);

		list = new QWidget(content);
		listLayout = new QVBoxLayout(list);
		listLayout->setContentsMargins(0, 0, 0, 0);
		listLayout->setSpacing(0);
		contentLayout->addWidget(list, 1);

		emptyLabel = new QLabel(content);
		setupEmptyLabel(emptyLabel);
		emptyLabel->setContentsMargins(16, 4, 12, 40);
		contentLayout->insertWidget(0, emptyLabel, 100);

		scroll->setWidget(content);
		layout->addWidget(scroll, 1);

		connect(editor, &ShotEditor::shotChanged, this, [this]() { updateName(); });
		connect(editor, &ShotEditor::layersChanged, this, [this]() {
			updateName();
			rebuild();
		});
		updateName();
		rebuild();
	}
};

/* ======================================================================== */
/* Shot Layer Properties                                                    */

class LayerPropertiesPage : public EditorPage {
	QDoubleSpinBox *posX;
	QDoubleSpinBox *posY;
	QDoubleSpinBox *sizeW;
	QDoubleSpinBox *sizeH;
	QComboBox *fill;
	SliderRow *rotation;
	SliderRow *opacity;
	SliderRow *crop[4];
	SliderRow *brightness;
	SliderRow *contrast;
	SliderRow *saturation;
	SliderRow *hue;
	bool percent = false;

	/* Canvas pixels per displayed unit */
	QPointF unitScale() const
	{
		const QSize canvas = canvasSize();
		return percent ? QPointF(canvas.width() / 100.0, canvas.height() / 100.0) : QPointF(1.0, 1.0);
	}

	void applyPosition()
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		const QSize canvas = canvasSize();
		const QPointF unit = unitScale();
		obs_transform_info info = centeredInfo(item);
		info.pos.x = float(canvas.width() / 2.0 + posX->value() * unit.x());
		info.pos.y = float(canvas.height() / 2.0 + posY->value() * unit.y());
		obs_sceneitem_set_info2(item, &info);
	}

	void applySize()
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		const QPointF unit = unitScale();
		obs_transform_info info = centeredInfo(item);
		if (info.bounds_type == OBS_BOUNDS_NONE) {
			info.bounds_type = kFillTypes[fill->currentIndex()];
		}
		info.bounds.x = float(std::max(1.0, sizeW->value() * unit.x()));
		info.bounds.y = float(std::max(1.0, sizeH->value() * unit.y()));
		obs_sceneitem_set_info2(item, &info);
	}

	void applyFill(int index)
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		obs_transform_info current;
		obs_sceneitem_get_info2(item, &current);
		const QSizeF size = itemSize(item, current);

		obs_transform_info info = centeredInfo(item);
		if (info.bounds_type == OBS_BOUNDS_NONE) {
			info.bounds.x = float(size.width());
			info.bounds.y = float(size.height());
		}
		info.bounds_type = kFillTypes[index];
		/* Fill Width / Fill Height hide what spills out of the box */
		info.crop_to_bounds = info.bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH ||
				      info.bounds_type == OBS_BOUNDS_SCALE_TO_HEIGHT;
		obs_sceneitem_set_info2(item, &info);
	}

	void scaleToFit()
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		const QSize canvas = canvasSize();
		obs_transform_info info = centeredInfo(item);
		if (info.bounds_type == OBS_BOUNDS_NONE) {
			info.bounds_type = kFillTypes[fill->currentIndex()];
		}
		info.bounds.x = float(canvas.width());
		info.bounds.y = float(canvas.height());
		info.pos.x = float(canvas.width()) / 2.0f;
		info.pos.y = float(canvas.height()) / 2.0f;
		info.rot = 0.0f;
		obs_sceneitem_set_info2(item, &info);
	}

	void resetPosition()
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		const QSize canvas = canvasSize();
		obs_transform_info info = centeredInfo(item);
		info.bounds_type = OBS_BOUNDS_NONE;
		info.crop_to_bounds = false;
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.pos.x = float(canvas.width()) / 2.0f;
		info.pos.y = float(canvas.height()) / 2.0f;
		info.rot = 0.0f;
		obs_sceneitem_set_info2(item, &info);
	}

	void applyRotation(double degrees)
	{
		if (OBSSceneItem item = editor->item()) {
			obs_transform_info info = centeredInfo(item);
			info.rot = float(degrees);
			obs_sceneitem_set_info2(item, &info);
		}
	}

	void applyCrop()
	{
		if (OBSSceneItem item = editor->item()) {
			obs_sceneitem_crop c;
			c.left = int(crop[0]->property("v").toDouble());
			c.right = int(crop[1]->property("v").toDouble());
			c.top = int(crop[2]->property("v").toDouble());
			c.bottom = int(crop[3]->property("v").toDouble());
			obs_sceneitem_set_crop(item, &c);
		}
	}

	void applyColor(const char *key, double value)
	{
		OBSSceneItem item = editor->item();
		if (!item) {
			return;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		OBSSource filter = findFilter(source, kColorFilterName);
		if (!filter) {
			filter = addFilter(source, kColorFilterId, kColorFilterName);
		}
		if (filter) {
			updateFilter(filter, [key, value](obs_data_t *settings) { obs_data_set_double(settings, key, value); });
		}
	}

	void resetColor()
	{
		if (OBSSceneItem item = editor->item()) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (OBSSource filter = findFilter(source, kColorFilterName)) {
				updateFilter(filter, [](obs_data_t *settings) {
					obs_data_set_double(settings, "brightness", 0.0);
					obs_data_set_double(settings, "contrast", 0.0);
					obs_data_set_double(settings, "saturation", 0.0);
					obs_data_set_double(settings, "hue_shift", 0.0);
				});
				read();
			}
		}
	}

public:
	LayerPropertiesPage(ShotEditor *editor_, QWidget *parent) : EditorPage(editor_, parent)
	{
		QWidget *main = new QWidget(body);
		QGridLayout *grid = newGrid(main);

		/* Position / Size: two numbers each, with the units gear beside Size */
		auto pair = [main](QDoubleSpinBox *&a, QDoubleSpinBox *&b, const char *la, const char *lb) {
			QWidget *w = new QWidget(main);
			QHBoxLayout *h = new QHBoxLayout(w);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(4);
			a = numberBox(w);
			b = numberBox(w);
			h->addWidget(new QLabel(QTStr(la), w));
			h->addWidget(a, 1);
			h->addSpacing(6);
			h->addWidget(new QLabel(QTStr(lb), w));
			h->addWidget(b, 1);
			return w;
		};

		addRow(grid, QTStr("Wirecast.ShotEditor.Position"),
		       pair(posX, posY, "Wirecast.ShotEditor.X", "Wirecast.ShotEditor.Y"));
		QWidget *sizeRow = pair(sizeW, sizeH, "Wirecast.ShotEditor.W", "Wirecast.ShotEditor.H");
		QToolButton *units = toolButton(Glyph::Gear, QTStr("Wirecast.ShotEditor.Units"), sizeRow);
		units->setPopupMode(QToolButton::InstantPopup);
		QMenu *unitsMenu = new QMenu(units);
		QAction *pixels = unitsMenu->addAction(QTStr("Wirecast.ShotEditor.Units.Pixels"));
		QAction *percentAction = unitsMenu->addAction(QTStr("Wirecast.ShotEditor.Units.Percent"));
		pixels->setCheckable(true);
		percentAction->setCheckable(true);
		pixels->setChecked(true);
		QActionGroup *unitGroup = new QActionGroup(unitsMenu);
		unitGroup->addAction(pixels);
		unitGroup->addAction(percentAction);
		units->setMenu(unitsMenu);
		static_cast<QHBoxLayout *>(sizeRow->layout())->addWidget(units);
		connect(pixels, &QAction::triggered, this, [this]() {
			percent = false;
			read();
		});
		connect(percentAction, &QAction::triggered, this, [this]() {
			percent = true;
			read();
		});
		addRow(grid, QTStr("Wirecast.ShotEditor.Size"), sizeRow);

		connect(posX, &QDoubleSpinBox::valueChanged, this, [this]() { applyPosition(); });
		connect(posY, &QDoubleSpinBox::valueChanged, this, [this]() { applyPosition(); });
		connect(sizeW, &QDoubleSpinBox::valueChanged, this, [this]() { applySize(); });
		connect(sizeH, &QDoubleSpinBox::valueChanged, this, [this]() { applySize(); });

		QWidget *buttons = new QWidget(main);
		QHBoxLayout *buttonLayout = new QHBoxLayout(buttons);
		buttonLayout->setContentsMargins(0, 0, 0, 0);
		buttonLayout->setSpacing(6);
		QPushButton *fit = new QPushButton(QTStr("Wirecast.ShotEditor.ScaleToFit"), buttons);
		QPushButton *reset = new QPushButton(QTStr("Wirecast.ShotEditor.ResetPosition"), buttons);
		buttonLayout->addWidget(fit);
		buttonLayout->addWidget(reset);
		buttonLayout->addStretch();
		grid->addWidget(buttons, grid->rowCount(), 1);
		connect(fit, &QPushButton::clicked, this, [this]() { scaleToFit(); });
		connect(reset, &QPushButton::clicked, this, [this]() { resetPosition(); });

		fill = new QComboBox(main);
		fill->addItem(QTStr("Wirecast.ShotEditor.Fill.Lock"));
		fill->addItem(QTStr("Wirecast.ShotEditor.Fill.Width"));
		fill->addItem(QTStr("Wirecast.ShotEditor.Fill.Height"));
		fill->addItem(QTStr("Wirecast.ShotEditor.Fill.Stretch"));
		addRow(grid, QTStr("Wirecast.ShotEditor.Fill"), fill);
		connect(fill, &QComboBox::activated, this, [this](int index) { applyFill(index); });

		rotation = new SliderRow(main, -180.0, 180.0, 0, QStringLiteral("°"));
		addRow(grid, QTStr("Wirecast.ShotEditor.Rotation"), rotation);
		rotation->changed = [this](double v) { applyRotation(v); };

		opacity = new SliderRow(main, 0.0, 100.0, 0, QStringLiteral("%"));
		addRow(grid, QTStr("Wirecast.ShotEditor.Opacity"), opacity);
		opacity->changed = [this](double v) { applyColor("opacity", v / 100.0); };

		bodyLayout->addWidget(main);

		/* Crop */
		QGridLayout *cropGrid = addSection(bodyLayout, body, QTStr("Wirecast.ShotEditor.Crop"));
		const char *cropNames[] = {"Wirecast.ShotEditor.Crop.Left", "Wirecast.ShotEditor.Crop.Right",
					   "Wirecast.ShotEditor.Crop.Top", "Wirecast.ShotEditor.Crop.Bottom"};
		QWidget *cropParent = cropGrid->parentWidget();
		for (int i = 0; i < 4; i++) {
			crop[i] = new SliderRow(cropParent, 0.0, 1920.0, 0);
			crop[i]->setProperty("v", 0.0);
			SliderRow *row = crop[i];
			row->changed = [this, row](double v) {
				row->setProperty("v", v);
				applyCrop();
			};
			addRow(cropGrid, QTStr(cropNames[i]), crop[i]);
		}
		QPushButton *cropReset = new QPushButton(QTStr("Wirecast.ShotEditor.Reset"), cropParent);
		cropGrid->addWidget(cropReset, cropGrid->rowCount(), 1, Qt::AlignLeft);
		connect(cropReset, &QPushButton::clicked, this, [this]() {
			for (SliderRow *row : crop) {
				row->setProperty("v", 0.0);
			}
			applyCrop();
		});

		/* Color */
		QGridLayout *colorGrid = addSection(bodyLayout, body, QTStr("Wirecast.ShotEditor.Color"));
		QWidget *colorParent = colorGrid->parentWidget();
		brightness = new SliderRow(colorParent, -1.0, 1.0, 2);
		contrast = new SliderRow(colorParent, -2.0, 2.0, 2);
		saturation = new SliderRow(colorParent, -1.0, 2.0, 2);
		hue = new SliderRow(colorParent, -180.0, 180.0, 0, QStringLiteral("°"));
		addRow(colorGrid, QTStr("Wirecast.ShotEditor.Brightness"), brightness);
		addRow(colorGrid, QTStr("Wirecast.ShotEditor.Contrast"), contrast);
		addRow(colorGrid, QTStr("Wirecast.ShotEditor.Saturation"), saturation);
		addRow(colorGrid, QTStr("Wirecast.ShotEditor.Hue"), hue);
		brightness->changed = [this](double v) { applyColor("brightness", v); };
		contrast->changed = [this](double v) { applyColor("contrast", v); };
		saturation->changed = [this](double v) { applyColor("saturation", v); };
		hue->changed = [this](double v) { applyColor("hue_shift", v); };
		QPushButton *colorReset = new QPushButton(QTStr("Wirecast.ShotEditor.Reset"), colorParent);
		colorGrid->addWidget(colorReset, colorGrid->rowCount(), 1, Qt::AlignLeft);
		connect(colorReset, &QPushButton::clicked, this, [this]() { resetColor(); });

		connect(editor, &ShotEditor::itemChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::shotChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::itemTransformedSignal, this, [this]() { read(); });
		read();
	}

	void read()
	{
		const QString empty = noLayerText();
		setEmpty(empty);
		if (!empty.isEmpty()) {
			return;
		}

		OBSSceneItem item = editor->item();
		obs_source_t *source = obs_sceneitem_get_source(item);

		obs_transform_info info;
		obs_sceneitem_get_info2(item, &info);
		const QSize canvas = canvasSize();
		const QPointF unit = unitScale();
		const QPointF center = itemCenter(item);
		const QSizeF size = itemSize(item, info);

		const int decimals = percent ? 1 : 0;
		for (QDoubleSpinBox *spin : {posX, posY, sizeW, sizeH}) {
			QSignalBlocker block(spin);
			spin->setDecimals(decimals);
		}
		setNumber(posX, (center.x() - canvas.width() / 2.0) / unit.x());
		setNumber(posY, (center.y() - canvas.height() / 2.0) / unit.y());
		setNumber(sizeW, size.width() / unit.x());
		setNumber(sizeH, size.height() / unit.y());

		{
			QSignalBlocker block(fill);
			fill->setCurrentIndex(fillIndex(info.bounds_type));
		}

		double rot = std::fmod(double(info.rot), 360.0);
		if (rot > 180.0) {
			rot -= 360.0;
		} else if (rot < -180.0) {
			rot += 360.0;
		}
		rotation->setValue(rot);

		obs_sceneitem_crop c;
		obs_sceneitem_get_crop(item, &c);
		const double maxX = std::max(1u, obs_source_get_width(source));
		const double maxY = std::max(1u, obs_source_get_height(source));
		const int cropValues[] = {c.left, c.right, c.top, c.bottom};
		for (int i = 0; i < 4; i++) {
			crop[i]->setRange(0.0, i < 2 ? maxX : maxY);
			crop[i]->setValue(cropValues[i]);
			crop[i]->setProperty("v", double(cropValues[i]));
		}

		double colorValues[] = {1.0, 0.0, 0.0, 0.0, 0.0};
		if (OBSSource filter = findFilter(source, kColorFilterName)) {
			OBSDataAutoRelease settings = obs_source_get_settings(filter);
			colorValues[0] = obs_data_get_double(settings, "opacity");
			colorValues[1] = obs_data_get_double(settings, "brightness");
			colorValues[2] = obs_data_get_double(settings, "contrast");
			colorValues[3] = obs_data_get_double(settings, "saturation");
			colorValues[4] = obs_data_get_double(settings, "hue_shift");
		}
		opacity->setValue(colorValues[0] * 100.0);
		brightness->setValue(colorValues[1]);
		contrast->setValue(colorValues[2]);
		saturation->setValue(colorValues[3]);
		hue->setValue(colorValues[4]);
	}
};

/* ======================================================================== */
/* Audio                                                                    */

class AudioPage : public EditorPage {
	QLabel *sourceName;
	QToolButton *mute;
	QHBoxLayout *meterLayout;
	QPointer<VolumeMeter> meter;
	SliderRow *volume;
	SliderRow *pan;
	QCheckBox *monitor;
	/* Held so its signal handler outlives our connections to it */
	OBSSource current;
	OBSSignal volumeSignal;
	OBSSignal muteSignal;
	OBSSignal balanceSignal;

	static constexpr double kMinDb = -60.0;
	static constexpr double kMaxDb = 20.0;

	static void audioChanged(void *data, calldata_t *)
	{
		AudioPage *page = static_cast<AudioPage *>(data);
		QMetaObject::invokeMethod(page, [page]() { page->readValues(); }, Qt::QueuedConnection);
	}

	OBSSource source() const
	{
		OBSSceneItem item = editor->item();
		return item ? OBSSource(obs_sceneitem_get_source(item)) : OBSSource();
	}

	void readValues()
	{
		OBSSource src = source();
		if (!src) {
			return;
		}
		const float mul = obs_source_get_volume(src);
		volume->setValue(mul > 0.0f ? std::clamp(20.0 * std::log10(double(mul)), kMinDb, kMaxDb) : kMinDb);
		pan->setValue(std::lround((double(obs_source_get_balance_value(src)) - 0.5) * 200.0));
		{
			QSignalBlocker block(mute);
			mute->setChecked(!obs_source_muted(src));
		}
		{
			QSignalBlocker block(monitor);
			monitor->setChecked(obs_source_get_monitoring_enabled(src));
		}
		if (meter) {
			meter->setMuted(obs_source_muted(src));
		}
	}

public:
	AudioPage(ShotEditor *editor_, QWidget *parent) : EditorPage(editor_, parent)
	{
		sourceName = new QLabel(body);
		setClasses(sourceName, "wc-editor-subtitle");
		bodyLayout->addWidget(sourceName);

		/* Speaker icon and meter */
		QWidget *meterRow = new QWidget(body);
		meterLayout = new QHBoxLayout(meterRow);
		meterLayout->setContentsMargins(0, 0, 0, 0);
		meterLayout->setSpacing(6);
		mute = new QToolButton(meterRow);
		mute->setCheckable(true);
		mute->setIcon(twoStateIcon(Glyph::OutputMuted, Glyph::OutputOn, 18));
		mute->setIconSize(QSize(18, 18));
		mute->setToolTip(QTStr("Wirecast.ShotEditor.LayerMute"));
		setClasses(mute, "wc-layer-toggle");
		meterLayout->addWidget(mute);
		bodyLayout->addWidget(meterRow);
		connect(mute, &QToolButton::toggled, this, [this](bool on) {
			if (OBSSource src = source()) {
				obs_source_set_muted(src, !on);
			}
		});

		QWidget *controls = new QWidget(body);
		QGridLayout *grid = newGrid(controls);

		volume = new SliderRow(controls, kMinDb, kMaxDb, 1, QStringLiteral(" dB"));
		volume->setSpecialValueText(QStringLiteral("-inf dB"));
		addRow(grid, QTStr("Wirecast.ShotEditor.Volume"), volume);
		volume->changed = [this](double db) {
			if (OBSSource src = source()) {
				obs_source_set_volume(src, db <= kMinDb ? 0.0f : float(std::pow(10.0, db / 20.0)));
			}
		};

		pan = new SliderRow(controls, -100.0, 100.0, 0);
		addRow(grid, QTStr("Wirecast.ShotEditor.Pan"), pan);
		pan->changed = [this](double v) {
			if (OBSSource src = source()) {
				obs_source_set_balance_value(src, float((v + 100.0) / 200.0));
			}
		};

		monitor = new QCheckBox(QTStr("Wirecast.ShotEditor.Monitor"), controls);
		grid->addWidget(monitor, grid->rowCount(), 1);
		connect(monitor, &QCheckBox::toggled, this, [this](bool on) {
			if (OBSSource src = source()) {
				obs_source_set_monitoring_enabled(src, on);
			}
		});

		QWidget *buttons = new QWidget(controls);
		QHBoxLayout *buttonLayout = new QHBoxLayout(buttons);
		buttonLayout->setContentsMargins(0, 0, 0, 0);
		buttonLayout->setSpacing(6);
		QPushButton *reset = new QPushButton(QTStr("Wirecast.ShotEditor.Reset"), buttons);
		QPushButton *map = new QPushButton(QTStr("Wirecast.ShotEditor.Map"), buttons);
		buttonLayout->addWidget(reset);
		buttonLayout->addWidget(map);
		buttonLayout->addStretch();
		grid->addWidget(buttons, grid->rowCount(), 1);
		connect(reset, &QPushButton::clicked, this, [this]() {
			if (OBSSource src = source()) {
				obs_source_set_volume(src, 1.0f);
				obs_source_set_balance_value(src, 0.5f);
				obs_source_set_muted(src, false);
				readValues();
			}
		});
		connect(map, &QPushButton::clicked, this, []() {
			if (OBSBasic *main = OBSBasic::Get()) {
				main->on_actionAdvAudioProperties_triggered();
			}
		});

		bodyLayout->addWidget(controls);

		connect(editor, &ShotEditor::itemChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::shotChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::layersChanged, this, [this]() { read(); });
		read();
	}

	void read()
	{
		QString empty = noLayerText();
		OBSSource src = source();
		if (empty.isEmpty() && !hasAudio(src)) {
			empty = emptyText("Wirecast.ShotEditor.NoAudio");
		}
		setEmpty(empty);

		if (!empty.isEmpty()) {
			src = nullptr;
		}

		if (src.Get() != current.Get()) {
			volumeSignal.Disconnect();
			muteSignal.Disconnect();
			balanceSignal.Disconnect();
			delete meter;
			current = src;

			if (src) {
				signal_handler_t *handler = obs_source_get_signal_handler(src);
				volumeSignal.Connect(handler, "volume", &AudioPage::audioChanged, this);
				muteSignal.Connect(handler, "mute", &AudioPage::audioChanged, this);
				balanceSignal.Connect(handler, "audio_balance", &AudioPage::audioChanged, this);
				meter = new VolumeMeter(body, src);
				meterLayout->addWidget(meter, 1);
			}
		}

		if (src) {
			sourceName->setText(QT_UTF8(obs_source_get_name(src)));
			readValues();
		}
	}
};

/* ======================================================================== */
/* Chroma Key Properties                                                    */

class ChromaKeyPage : public EditorPage {
	QCheckBox *use;
	QComboBox *type;
	QComboBox *colorType;
	QPushButton *colorButton;
	QWidget *colorRow;
	SliderRow *threshold;
	SliderRow *smoothness;
	SliderRow *spill;
	SliderRow *blackClip;
	SliderRow *blackSmooth;
	SliderRow *whiteClip;
	SliderRow *whiteSmooth;
	QList<QWidget *> chromaRows;
	QList<QWidget *> lumaRows;
	QList<QWidget *> spillRows;
	QColor customColor = QColor(0, 255, 0);

	OBSSource layerSource() const
	{
		OBSSceneItem item = editor->item();
		return item ? OBSSource(obs_sceneitem_get_source(item)) : OBSSource();
	}

	/* The key filter of the current type, created (enabled) if needed */
	OBSSource keyFilter(bool create)
	{
		OBSSource source = layerSource();
		if (!source) {
			return nullptr;
		}
		const char *id = kKeyFilterIds[type->currentIndex()];
		OBSSource filter = findKeyFilter(source);
		if (filter && strcmp(obs_source_get_id(filter), id) != 0) {
			if (!create) {
				return nullptr;
			}
			/* Switching key type replaces the filter */
			const std::string name = obs_source_get_name(filter);
			obs_source_filter_remove(source, filter);
			filter = addFilter(source, id, name.c_str());
		} else if (!filter && create) {
			filter = addFilter(source, id, kKeyFilterName);
		}
		return filter;
	}

	void set(const std::function<void(obs_data_t *)> &apply)
	{
		if (!use->isChecked()) {
			use->setChecked(true);
		}
		if (OBSSource filter = keyFilter(true)) {
			obs_source_set_enabled(filter, true);
			updateFilter(filter, apply);
		}
	}

	void updateVisibility()
	{
		const int index = type->currentIndex();
		for (QWidget *w : chromaRows) {
			w->setVisible(index != 1);
		}
		for (QWidget *w : spillRows) {
			w->setVisible(index == 0);
		}
		for (QWidget *w : lumaRows) {
			w->setVisible(index == 1);
		}
		colorButton->setVisible(colorType->currentIndex() == 3);
	}

	void updateColorButton()
	{
		colorButton->setStyleSheet(QString("background: %1;").arg(customColor.name()));
	}

	void addGridRow(QGridLayout *grid, const QString &label, QWidget *widget, QList<QWidget *> &group)
	{
		const int row = grid->rowCount();
		QLabel *l = rowLabel(label, widget->parentWidget());
		grid->addWidget(l, row, 0);
		grid->addWidget(widget, row, 1);
		group << l << widget;
	}

public:
	ChromaKeyPage(ShotEditor *editor_, QWidget *parent) : EditorPage(editor_, parent)
	{
		QWidget *controls = new QWidget(body);
		QGridLayout *grid = newGrid(controls);

		QWidget *useRow = new QWidget(controls);
		QHBoxLayout *useLayout = new QHBoxLayout(useRow);
		useLayout->setContentsMargins(0, 0, 0, 0);
		useLayout->setSpacing(8);
		use = new QCheckBox(QTStr("Wirecast.ShotEditor.Use"), useRow);
		type = new QComboBox(useRow);
		type->addItem(QTStr("Wirecast.ShotEditor.Key.Chroma"));
		type->addItem(QTStr("Wirecast.ShotEditor.Key.Luma"));
		type->addItem(QTStr("Wirecast.ShotEditor.Key.Color"));
		useLayout->addWidget(use);
		useLayout->addWidget(type, 1);
		grid->addWidget(useRow, 0, 0, 1, 2);

		colorRow = new QWidget(controls);
		QHBoxLayout *colorLayout = new QHBoxLayout(colorRow);
		colorLayout->setContentsMargins(0, 0, 0, 0);
		colorLayout->setSpacing(6);
		colorType = new QComboBox(colorRow);
		colorType->addItem(QTStr("Wirecast.ShotEditor.KeyColor.Green"));
		colorType->addItem(QTStr("Wirecast.ShotEditor.KeyColor.Blue"));
		colorType->addItem(QTStr("Wirecast.ShotEditor.KeyColor.Magenta"));
		colorType->addItem(QTStr("Wirecast.ShotEditor.KeyColor.Custom"));
		colorButton = new QPushButton(colorRow);
		colorButton->setObjectName("wcKeyColorSwatch");
		colorButton->setFixedSize(36, 20);
		colorButton->setToolTip(QTStr("Wirecast.ShotEditor.KeyColor.Pick"));
		colorLayout->addWidget(colorType, 1);
		colorLayout->addWidget(colorButton);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.KeyColor"), colorRow, chromaRows);

		threshold = new SliderRow(controls, 1.0, 1000.0, 0);
		smoothness = new SliderRow(controls, 1.0, 1000.0, 0);
		spill = new SliderRow(controls, 1.0, 1000.0, 0);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.KeyThreshold"), threshold, chromaRows);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.Smoothness"), smoothness, chromaRows);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.Spill"), spill, spillRows);

		blackClip = new SliderRow(controls, 0.0, 1.0, 3);
		blackSmooth = new SliderRow(controls, 0.0, 1.0, 3);
		whiteClip = new SliderRow(controls, 0.0, 1.0, 3);
		whiteSmooth = new SliderRow(controls, 0.0, 1.0, 3);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.BlackClip"), blackClip, lumaRows);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.BlackSmooth"), blackSmooth, lumaRows);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.WhiteClip"), whiteClip, lumaRows);
		addGridRow(grid, QTStr("Wirecast.ShotEditor.WhiteSmooth"), whiteSmooth, lumaRows);

		bodyLayout->addWidget(controls);

		connect(use, &QCheckBox::toggled, this, [this](bool on) {
			if (on) {
				if (OBSSource filter = keyFilter(true)) {
					obs_source_set_enabled(filter, true);
				}
			} else if (OBSSource filter = keyFilter(false)) {
				obs_source_set_enabled(filter, false);
			}
		});
		connect(type, &QComboBox::activated, this, [this]() {
			if (use->isChecked()) {
				keyFilter(true);
			}
			read();
		});
		connect(colorType, &QComboBox::activated, this, [this](int index) {
			set([index, this](obs_data_t *s) {
				obs_data_set_string(s, "key_color_type", kKeyColorTypes[index]);
				if (index == 3) {
					obs_data_set_int(s, "key_color",
							 customColor.red() | (customColor.green() << 8) |
								 (customColor.blue() << 16));
				}
			});
			updateVisibility();
		});
		connect(colorButton, &QPushButton::clicked, this, [this]() {
			QColor color = QColorDialog::getColor(customColor, this, QTStr("Wirecast.ShotEditor.KeyColor.Pick"));
			if (!color.isValid()) {
				return;
			}
			customColor = color;
			updateColorButton();
			set([color](obs_data_t *s) {
				obs_data_set_string(s, "key_color_type", "custom");
				obs_data_set_int(s, "key_color", color.red() | (color.green() << 8) | (color.blue() << 16));
			});
		});

		threshold->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_int(s, "similarity", std::lround(v)); });
		};
		smoothness->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_int(s, "smoothness", std::lround(v)); });
		};
		spill->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_int(s, "spill", std::lround(v)); });
		};
		blackClip->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_double(s, "luma_min", v); });
		};
		blackSmooth->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_double(s, "luma_min_smooth", v); });
		};
		whiteClip->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_double(s, "luma_max", v); });
		};
		whiteSmooth->changed = [this](double v) {
			set([v](obs_data_t *s) { obs_data_set_double(s, "luma_max_smooth", v); });
		};

		connect(editor, &ShotEditor::itemChanged, this, [this]() { read(true); });
		connect(editor, &ShotEditor::shotChanged, this, [this]() { read(true); });
		read(true);
	}

	void read(bool fromFilter = false)
	{
		const QString empty = noLayerText();
		setEmpty(empty);
		if (!empty.isEmpty()) {
			return;
		}

		OBSSource source = layerSource();
		OBSSource filter = findKeyFilter(source);
		if (fromFilter) {
			QSignalBlocker block(type);
			type->setCurrentIndex(keyTypeIndex(filter));
		}
		if (filter && strcmp(obs_source_get_id(filter), kKeyFilterIds[type->currentIndex()]) != 0) {
			filter = nullptr;
		}

		{
			QSignalBlocker block(use);
			use->setChecked(filter && obs_source_enabled(filter));
		}

		OBSDataAutoRelease settings = filter ? obs_source_get_settings(filter)
						     : obs_get_source_defaults(kKeyFilterIds[type->currentIndex()]);

		const char *colorTypeName = obs_data_get_string(settings, "key_color_type");
		int colorIndex = 0;
		for (int i = 0; i < int(std::size(kKeyColorTypes)); i++) {
			if (strcmp(colorTypeName, kKeyColorTypes[i]) == 0) {
				colorIndex = i;
			}
		}
		{
			QSignalBlocker block(colorType);
			colorType->setCurrentIndex(colorIndex);
		}
		const uint32_t rgb = uint32_t(obs_data_get_int(settings, "key_color"));
		customColor = QColor(int(rgb & 0xFF), int((rgb >> 8) & 0xFF), int((rgb >> 16) & 0xFF));
		updateColorButton();

		threshold->setValue(double(obs_data_get_int(settings, "similarity")));
		smoothness->setValue(double(obs_data_get_int(settings, "smoothness")));
		spill->setValue(double(obs_data_get_int(settings, "spill")));
		blackClip->setValue(obs_data_get_double(settings, "luma_min"));
		blackSmooth->setValue(obs_data_get_double(settings, "luma_min_smooth"));
		whiteClip->setValue(obs_data_get_double(settings, "luma_max"));
		whiteSmooth->setValue(obs_data_get_double(settings, "luma_max_smooth"));

		updateVisibility();
	}
};

/* ======================================================================== */
/* Smooth Transition Properties                                             */

/* Directions in combo order; Build In comes *from* a side, Build Out goes *to* one */
constexpr const char *kBuildInSlide[] = {nullptr, "right", "left", "down", "up"};
constexpr const char *kBuildOutSlide[] = {nullptr, "left", "right", "up", "down"};

class SmoothTransitionPage : public EditorPage {
	struct Build {
		QComboBox *direction;
		QCheckBox *fade;
		QSpinBox *duration;
	};
	Build in;
	Build out;

	Build makeBuild(QGridLayout *grid, QWidget *parent, bool show)
	{
		Build b;
		b.direction = new QComboBox(parent);
		const char *prefix = show ? "Wirecast.ShotEditor.Dir.From" : "Wirecast.ShotEditor.Dir.To";
		b.direction->addItem(QTStr("Wirecast.ShotEditor.Dir.None"));
		for (const char *side : {"Left", "Right", "Top", "Bottom"}) {
			b.direction->addItem(QTStr((std::string(prefix) + side).c_str()));
		}
		b.fade = new QCheckBox(QTStr(show ? "Wirecast.ShotEditor.FadeIn" : "Wirecast.ShotEditor.FadeOut"),
				       parent);
		b.duration = new QSpinBox(parent);
		b.duration->setRange(50, 10000);
		b.duration->setSingleStep(50);
		b.duration->setSuffix(QStringLiteral(" ms"));

		addRow(grid, QTStr("Wirecast.ShotEditor.Direction"), b.direction);
		grid->addWidget(b.fade, grid->rowCount(), 1);
		addRow(grid, QTStr("Wirecast.ShotEditor.Duration"), b.duration);

		connect(b.direction, &QComboBox::activated, this, [this, show]() {
			if (OBSSceneItem item = editor->item()) {
				apply(item, show);
			}
			updateEnabled();
		});
		connect(b.fade, &QCheckBox::toggled, this, [this, show]() {
			if (OBSSceneItem item = editor->item()) {
				apply(item, show);
			}
		});
		connect(b.duration, &QSpinBox::valueChanged, this, [this, show]() {
			if (OBSSceneItem item = editor->item()) {
				apply(item, show);
			}
		});
		return b;
	}

	void apply(obs_sceneitem_t *item, bool show)
	{
		const Build &b = show ? in : out;
		const int dir = b.direction->currentIndex();

		OBSSourceAutoRelease transition;
		if (dir > 0) {
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_string(settings, "direction", show ? kBuildInSlide[dir] : kBuildOutSlide[dir]);
			transition = obs_source_create_private("slide_transition", "Wirecast Build", settings);
		} else if (b.fade->isChecked()) {
			transition = obs_source_create_private("fade_transition", "Wirecast Build", nullptr);
		}

		obs_sceneitem_set_transition(item, show, transition);
		obs_sceneitem_set_transition_duration(item, show, uint32_t(b.duration->value()));
	}

	void readBuild(obs_sceneitem_t *item, bool show)
	{
		const Build &b = show ? in : out;
		obs_source_t *transition = obs_sceneitem_get_transition(item, show);

		int dir = 0;
		bool fade = false;
		if (transition) {
			const char *id = obs_source_get_unversioned_id(transition);
			if (strcmp(id, "slide_transition") == 0) {
				OBSDataAutoRelease settings = obs_source_get_settings(transition);
				const char *direction = obs_data_get_string(settings, "direction");
				const char *const *table = show ? kBuildInSlide : kBuildOutSlide;
				for (int i = 1; i < int(std::size(kBuildInSlide)); i++) {
					if (strcmp(direction, table[i]) == 0) {
						dir = i;
					}
				}
			} else if (strcmp(id, "fade_transition") == 0) {
				fade = true;
			}
		}

		QSignalBlocker blockDir(b.direction);
		QSignalBlocker blockFade(b.fade);
		QSignalBlocker blockDuration(b.duration);
		b.direction->setCurrentIndex(dir);
		b.fade->setChecked(fade);
		/* Layers without a build yet show Wirecast's normal speed */
		const uint32_t duration = obs_sceneitem_get_transition_duration(item, show);
		b.duration->setValue(transition && duration > 0 ? int(duration) : 500);
	}

	void updateEnabled()
	{
		/* A slide replaces the fade; OBS layers play one transition at a time */
		in.fade->setEnabled(in.direction->currentIndex() == 0);
		out.fade->setEnabled(out.direction->currentIndex() == 0);
	}

public:
	SmoothTransitionPage(ShotEditor *editor_, QWidget *parent) : EditorPage(editor_, parent)
	{
		QLabel *note = new QLabel(QTStr("Wirecast.ShotEditor.Transition.Note"), body);
		note->setWordWrap(true);
		setClasses(note, "wc-editor-empty");
		bodyLayout->addWidget(note);

		QGridLayout *inGrid = addSection(bodyLayout, body, QTStr("Wirecast.ShotEditor.BuildIn"), true);
		in = makeBuild(inGrid, inGrid->parentWidget(), true);
		QGridLayout *outGrid = addSection(bodyLayout, body, QTStr("Wirecast.ShotEditor.BuildOut"), true);
		out = makeBuild(outGrid, outGrid->parentWidget(), false);

		QPushButton *applyAll = new QPushButton(QTStr("Wirecast.ShotEditor.ApplyToAll"), body);
		applyAll->setToolTip(QTStr("Wirecast.ShotEditor.ApplyToAll.Tooltip"));
		bodyLayout->addWidget(applyAll, 0, Qt::AlignLeft);
		connect(applyAll, &QPushButton::clicked, this, [this]() {
			for (const OBSSceneItem &item : sceneItems(editor->shotScene())) {
				apply(item, true);
				apply(item, false);
			}
		});

		connect(editor, &ShotEditor::itemChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::shotChanged, this, [this]() { read(); });
		read();
	}

	void read()
	{
		const QString empty = noLayerText();
		setEmpty(empty);
		if (!empty.isEmpty()) {
			return;
		}
		OBSSceneItem item = editor->item();
		readBuild(item, true);
		readBuild(item, false);
		updateEnabled();
	}
};

/* ======================================================================== */
/* Source Properties (...)                                                  */

class SourcePropertiesPage : public QWidget {
	ShotEditor *editor;
	QLineEdit *nameEdit;
	QWidget *nameRow;
	MediaControls *media;
	QLabel *emptyLabel;
	QVBoxLayout *layout;
	QPointer<OBSPropertiesView> view;
	/* Held so the media controls' signal connections stay valid */
	OBSSource current;

	OBSSource layerSource() const
	{
		OBSSceneItem item = editor->item();
		return item ? OBSSource(obs_sceneitem_get_source(item)) : OBSSource();
	}

	void rename()
	{
		OBSSource source = layerSource();
		if (!source) {
			return;
		}
		const std::string name = QT_TO_UTF8(nameEdit->text().trimmed());
		if (!name.empty() && name != obs_source_get_name(source)) {
			OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
			if (!existing) {
				obs_source_set_name(source, name.c_str());
			}
		}
		nameEdit->clearFocus();
		read();
	}

public:
	SourcePropertiesPage(ShotEditor *editor_, QWidget *parent) : QWidget(parent), editor(editor_)
	{
		setObjectName("wcSourcePropertiesPage");
		layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);

		nameRow = new QWidget(this);
		nameRow->setObjectName("wirecastShotNameRow");
		QHBoxLayout *nameLayout = new QHBoxLayout(nameRow);
		nameLayout->setContentsMargins(16, 0, 8, 2);
		nameLayout->setSpacing(6);
		nameEdit = new QLineEdit(nameRow);
		nameEdit->setObjectName("wirecastShotName");
		nameLayout->addWidget(new QLabel(QTStr("Wirecast.ShotName"), nameRow));
		nameLayout->addWidget(nameEdit, 1);
		layout->addWidget(nameRow);
		connect(nameEdit, &QLineEdit::editingFinished, this, [this]() { rename(); });

		media = new MediaControls(this);
		media->setContentsMargins(12, 0, 8, 0);
		layout->addWidget(media);

		emptyLabel = new QLabel(this);
		setupEmptyLabel(emptyLabel);
		emptyLabel->setContentsMargins(16, 4, 12, 40);
		layout->addWidget(emptyLabel, 100);
		layout->addStretch();

		connect(editor, &ShotEditor::itemChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::shotChanged, this, [this]() { read(); });
		connect(editor, &ShotEditor::layersChanged, this, [this]() { read(); });
		read();
	}

	void read()
	{
		OBSSource source = layerSource();
		if (!editor->shot()) {
			emptyLabel->setText(emptyText("Wirecast.ShotEditor.NoShot"));
		} else {
			emptyLabel->setText(emptyText("Wirecast.ShotEditor.NoLayer"));
		}
		emptyLabel->setVisible(!source);
		nameRow->setVisible(!!source);

		if (source.Get() != current.Get()) {
			delete view;

			const bool controllable =
				source && (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0;
			media->SetSource(controllable ? source : OBSSource());
			media->setVisible(controllable);
			current = source;

			if (source && obs_source_configurable(source)) {
				OBSDataAutoRelease settings = obs_source_get_settings(source);
				view = new OBSPropertiesView(settings.Get(), source.Get(),
							     (PropertiesReloadCallback)obs_source_properties,
							     (PropertiesUpdateCallback) nullptr,
							     (PropertiesVisualUpdateCb)obs_source_update);
				view->setObjectName("wcSourcePropertiesView");
				view->setMinimumHeight(150);
				/* Before the stretch */
				layout->insertWidget(layout->count() - 1, view, 1);
			}
		}

		if (source && !nameEdit->hasFocus()) {
			nameEdit->setText(QT_UTF8(obs_source_get_name(source)));
		}
	}
};

} // namespace

/* ======================================================================== */
/* ShotEditor                                                               */

ShotEditor::ShotEditor(QObject *parent) : QObject(parent)
{
	layersTimer.setSingleShot(true);
	layersTimer.setInterval(0);
	connect(&layersTimer, &QTimer::timeout, this, [this]() {
		ensureItem();
		emit layersChanged();
	});

	/* Dragging in the Preview sends many transform updates; refresh the
	 * numbers at most every few frames */
	transformTimer.setSingleShot(true);
	transformTimer.setInterval(30);
	connect(&transformTimer, &QTimer::timeout, this, [this]() { emit itemTransformedSignal(); });

	renameSignal.Connect(obs_get_signal_handler(), "source_rename", &ShotEditor::sourceRenamed, this);
	obs_frontend_add_event_callback(&ShotEditor::onFrontendEvent, this);
	eventCallbackRegistered = true;
}

ShotEditor::~ShotEditor()
{
	if (eventCallbackRegistered) {
		obs_frontend_remove_event_callback(&ShotEditor::onFrontendEvent, this);
	}
	shotSignals.clear();
}

QWidget *ShotEditor::createPage(Page page, QWidget *parent)
{
	switch (page) {
	case Page::Layers:
		return new ShotLayersPage(this, parent);
	case Page::LayerProperties:
		return new LayerPropertiesPage(this, parent);
	case Page::Audio:
		return new AudioPage(this, parent);
	case Page::ChromaKey:
		return new ChromaKeyPage(this, parent);
	case Page::SmoothTransition:
		return new SmoothTransitionPage(this, parent);
	case Page::SourceProperties:
		return new SourcePropertiesPage(this, parent);
	}
	return nullptr;
}

OBSSource ShotEditor::shot() const
{
	return heldShot;
}

OBSScene ShotEditor::shotScene() const
{
	return heldShot ? OBSScene(obs_scene_from_source(heldShot)) : OBSScene();
}

OBSSceneItem ShotEditor::item() const
{
	OBSScene scene = shotScene();
	if (!scene || !itemId) {
		return nullptr;
	}
	return obs_scene_find_sceneitem_by_id(scene, itemId);
}

void ShotEditor::selectItem(obs_sceneitem_t *item)
{
	const int64_t id = item ? obs_sceneitem_get_id(item) : 0;
	if (id == itemId) {
		return;
	}
	itemId = id;
	emit itemChanged();
	emit layersChanged();
	emit sourcePageTitleChanged(sourcePageTitle());
}

QString ShotEditor::sourcePageTitle() const
{
	OBSSceneItem selected = item();
	if (!selected) {
		return QTStr("Wirecast.ShotEditor.SourceProperties");
	}
	obs_source_t *source = obs_sceneitem_get_source(selected);
	return QTStr("Wirecast.ShotEditor.SourcePropertiesOf")
		.arg(QT_UTF8(obs_source_get_display_name(obs_source_get_id(source))));
}

void ShotEditor::refreshShot()
{
	OBSSource selected = LayersPanel::selectedShot();
	if (selected && obs_source_removed(selected)) {
		selected = nullptr;
	}
	if (selected.Get() != heldShot.Get()) {
		setShot(selected);
	}
}

void ShotEditor::setShot(obs_source_t *shot)
{
	shotSignals.clear();
	heldShot = shot;
	itemId = 0;

	if (shot) {
		signal_handler_t *handler = obs_source_get_signal_handler(shot);
		for (const char *signal : {"item_add", "item_remove", "reorder", "refresh", "item_visible"}) {
			shotSignals.emplace_back(handler, signal, &ShotEditor::sceneChanged, this);
		}
		shotSignals.emplace_back(handler, "item_transform", &ShotEditor::itemTransformed, this);
		shotSignals.emplace_back(handler, "remove", &ShotEditor::shotRemoved, this);
	}

	ensureItem();
	emit shotChanged();
	emit itemChanged();
	emit layersChanged();
	emit sourcePageTitleChanged(sourcePageTitle());

	/* A shot that is still being filled (new shot, imported media) gets its
	 * layers listed once more after it is complete */
	layersTimer.start();
}

void ShotEditor::ensureItem()
{
	if (item()) {
		return;
	}

	/* Select the top layer, as Wirecast does when a shot opens */
	std::vector<OBSSceneItem> items = sceneItems(shotScene());
	const int64_t id = items.empty() ? 0 : obs_sceneitem_get_id(items.back());
	if (id != itemId) {
		itemId = id;
		emit itemChanged();
		emit sourcePageTitleChanged(sourcePageTitle());
	}
}

void ShotEditor::emitLayersChanged()
{
	layersTimer.start();
}

void ShotEditor::emitTransformed()
{
	if (!transformTimer.isActive()) {
		transformTimer.start();
	}
}

void ShotEditor::sceneChanged(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<ShotEditor *>(data), "emitLayersChanged", Qt::QueuedConnection);
}

void ShotEditor::itemTransformed(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<ShotEditor *>(data), "emitTransformed", Qt::QueuedConnection);
}

void ShotEditor::shotRemoved(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<ShotEditor *>(data), "refreshShot", Qt::QueuedConnection);
}

void ShotEditor::sourceRenamed(void *data, calldata_t *)
{
	ShotEditor *editor = static_cast<ShotEditor *>(data);
	QMetaObject::invokeMethod(
		editor,
		[editor]() {
			emit editor->shotChanged();
			editor->emitLayersChanged();
		},
		Qt::QueuedConnection);
}

void ShotEditor::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	ShotEditor *editor = static_cast<ShotEditor *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		/* Let go of the shot so the collection can be freed */
		editor->setShot(nullptr);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		editor->setShot(nullptr);
		editor->renameSignal.Disconnect();
		obs_frontend_remove_event_callback(&ShotEditor::onFrontendEvent, editor);
		editor->eventCallbackRegistered = false;
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		editor->refreshShot();
		break;
	default:
		break;
	}
}
