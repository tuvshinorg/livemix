/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "OBSBasic.hpp"
#include "OBSProjector.hpp"

#include <utility/display-helpers.hpp>
#include <components/WirecastAudioMeter.hpp>
#include <components/WirecastIcons.hpp>
#include <utility/MonitorBorder.hpp>
#include <utility/QuickTransition.hpp>

#include <qt-wrappers.hpp>
#include <slider-ignorewheel.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

/* Height kept free under each monitor picture for the buttons Wirecast puts there */
constexpr int kMonitorButtonRow = 28;

/* Holds the Live display sized to the picture, with the snapshot button just under its
 * bottom-left corner as in Wirecast (no empty display area for the button to float in). */
/* Anything visible in the scene, looking inside nested scenes (the Layer Mix holds shots) */
bool sceneHasContent(obs_scene_t *scene, int depth = 0)
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
			ctx->found = child ? sceneHasContent(child, ctx->depth + 1) : true;
			return !ctx->found;
		},
		&ctx);
	return ctx.found;
}

/* Shown in the Live monitor while nothing is live, like Wirecast's
 * "Live Broadcast Area" card */
class LivePlaceholder : public QWidget {
public:
	explicit LivePlaceholder(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_OpaquePaintEvent);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		/* A quiet empty monitor: dark picture, two lines of muted text, the red Live outline */
		QPainter p(this);
		const QRect r = rect();
		p.fillRect(r, QColor(0x0D, 0x0E, 0x0F));
		p.setRenderHint(QPainter::Antialiasing);

		const int titleSize = std::clamp(r.height() / 16, 11, 22);
		const int textSize = std::clamp(r.height() / 24, 9, 15);
		const int titleY = r.height() / 2 - titleSize;

		QFont font = p.font();
		font.setPixelSize(titleSize);
		font.setWeight(QFont::Medium);
		p.setFont(font);
		p.setPen(QColor(0xA9, 0xA6, 0x9E));
		p.drawText(QRect(0, titleY, r.width(), titleSize + titleSize / 2), Qt::AlignHCenter | Qt::AlignVCenter,
			   QTStr("Wirecast.LiveArea.Title"));

		font.setPixelSize(textSize);
		font.setWeight(QFont::Normal);
		p.setFont(font);
		p.setPen(QColor(0x75, 0x72, 0x6B));
		p.drawText(QRect(0, titleY + titleSize + titleSize / 2, r.width(), textSize * 2),
			   Qt::AlignHCenter | Qt::AlignTop, QTStr("Wirecast.LiveArea.Text"));

		p.setPen(QPen(WirecastIcons::kRed, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(r).adjusted(0.75, 0.75, -0.75, -0.75), 2, 2);
	}
};

class LiveMonitorFrame : public QWidget {
public:
	LiveMonitorFrame(QWidget *display_, QWidget *button_) : display(display_), button(button_)
	{
		display->setParent(this);
		button->setParent(this);
		placeholder = new LivePlaceholder(this);
		placeholder->hide();
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMinimumSize(32, 32 + kMonitorButtonRow * 2);

		/* Swap in the placeholder while the program has nothing to show */
		emptyTimer.setInterval(500);
		QObject::connect(&emptyTimer, &QTimer::timeout, this, [this]() { updateEmpty(); });
		emptyTimer.start();
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		layoutChildren();
	}

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		layoutChildren();
	}

private:
	/* The program display can be deleted before this frame while OBS shuts down */
	QPointer<QWidget> display;
	QPointer<QWidget> button;
	LivePlaceholder *placeholder;
	QTimer emptyTimer;
	bool empty = false;

	void updateEmpty()
	{
		if (!display || !button) {
			emptyTimer.stop();
			return;
		}

		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		const bool nowEmpty = !sceneHasContent(obs_scene_from_source(program));
		if (nowEmpty == empty) {
			return;
		}
		empty = nowEmpty;
		placeholder->setVisible(empty);
		display->setVisible(!empty);
		layoutChildren();
	}

	void layoutChildren()
	{
		if (!display || !button) {
			return;
		}

		obs_video_info ovi;
		const double aspect = obs_get_video_info(&ovi) && ovi.base_height
					      ? double(ovi.base_width) / double(ovi.base_height)
					      : 16.0 / 9.0;

		/* Same box the Preview picture fits in (its grid keeps the button row free top and bottom) */
		const int edge = int(std::ceil(PREVIEW_EDGE_SIZE / devicePixelRatioF()));
		const int availW = std::max(width() - edge * 2, 1);
		const int availH = std::max(height() - kMonitorButtonRow * 2 - edge * 2, 1);
		int picW = availW;
		int picH = int(picW / aspect);
		if (picH > availH) {
			picH = availH;
			picW = int(picH * aspect);
		}

		/* Top-aligned with the Preview picture, as in Wirecast */
		const int x = (width() - picW) / 2;
		const int y = kMonitorButtonRow + edge;
		display->setGeometry(x - edge, y - edge, picW + edge * 2, picH + edge * 2);
		placeholder->setGeometry(x, y, picW, picH);

		/* Below the display's edge margin, which the native display paints over */
		const QSize hint = button->sizeHint();
		button->setGeometry(x - 3, y + picH + edge + 1, hint.width(), hint.height());
		button->raise();
	}
};

/* The "|||" handle between Preview and Live: drag it left or right to make one monitor
 * bigger and the other smaller, double-click to make them equal again. */
class MonitorSplitGrip : public QLabel {
public:
	MonitorSplitGrip(QBoxLayout *layout_, QWidget *left_, QWidget *right_, QWidget *parent)
		: QLabel(QStringLiteral("|||"), parent),
		  layout(layout_),
		  left(left_),
		  right(right_)
	{
		setObjectName("wirecastMonitorGrip");
		setAlignment(Qt::AlignCenter);
		setCursor(Qt::SplitHCursor);
		setToolTip(QTStr("Wirecast.MonitorGrip.Tooltip"));

		/* Width comes only from the split, not from what each side would like */
		left->setSizePolicy(QSizePolicy::Ignored, left->sizePolicy().verticalPolicy());
		right->setSizePolicy(QSizePolicy::Ignored, right->sizePolicy().verticalPolicy());

		setSplit(config_get_double(App()->GetUserConfig(), "BasicWindow", "WirecastMonitorSplit"));
	}

	~MonitorSplitGrip() override
	{
		/* Back to normal for the single monitor outside studio mode */
		if (layout && left) {
			layout->setStretchFactor(left, 0);
			left->setSizePolicy(QSizePolicy::Expanding, left->sizePolicy().verticalPolicy());
		}
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton) {
			QLabel::mousePressEvent(event);
			return;
		}
		dragging = true;
		dragStartX = event->globalPosition().x();
		dragStartLeft = left->width();
		dragTotal = left->width() + right->width();
		setProperty("dragging", true);
		style()->unpolish(this);
		style()->polish(this);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!dragging || dragTotal <= 0) {
			return;
		}
		const double newLeft = dragStartLeft + (event->globalPosition().x() - dragStartX);
		setSplit(newLeft / dragTotal);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (!dragging) {
			QLabel::mouseReleaseEvent(event);
			return;
		}
		dragging = false;
		setProperty("dragging", false);
		style()->unpolish(this);
		style()->polish(this);
		config_set_double(App()->GetUserConfig(), "BasicWindow", "WirecastMonitorSplit", split);
	}

	void mouseDoubleClickEvent(QMouseEvent *) override
	{
		setSplit(0.5);
		config_set_double(App()->GetUserConfig(), "BasicWindow", "WirecastMonitorSplit", split);
	}

private:
	QPointer<QBoxLayout> layout;
	QPointer<QWidget> left;
	QPointer<QWidget> right;
	double split = 0.5;
	bool dragging = false;
	double dragStartX = 0.0;
	int dragStartLeft = 0;
	int dragTotal = 0;

	void setSplit(double value)
	{
		split = (value > 0.0) ? std::clamp(value, 0.15, 0.85) : 0.5;
		const int leftStretch = int(std::lround(split * 1000.0));
		layout->setStretchFactor(left, leftStretch);
		layout->setStretchFactor(right, 1000 - leftStretch);
	}
};

} // namespace

void OBSBasic::CreateProgramDisplay()
{
	program = new OBSQTDisplay();

	program->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(program.data(), &QWidget::customContextMenuRequested, this, &OBSBasic::ProgramViewContextMenuRequested);

	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi)) {
			ResizeProgram(ovi.base_width, ovi.base_height);
		}
	};

	connect(program.data(), &OBSQTDisplay::DisplayResized, this, displayResize);

	auto addDisplay = [this](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(), OBSBasic::RenderProgram, this);

		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			ResizeProgram(ovi.base_width, ovi.base_height);
		}
	};

	connect(program.data(), &OBSQTDisplay::DisplayCreated, this, addDisplay);

	program->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

#define T_BAR_PRECISION 1024
#define T_BAR_PRECISION_F ((float)T_BAR_PRECISION)
#define T_BAR_CLAMP (T_BAR_PRECISION / 10)

void OBSBasic::CreateProgramOptions()
{
	programOptions = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout();
	layout->setSpacing(4);

	QPushButton *configTransitions = new QPushButton();
	configTransitions->setProperty("class", "icon-dots-vert");

	QHBoxLayout *mainButtonLayout = new QHBoxLayout();
	mainButtonLayout->setSpacing(2);

	transitionButton = new QPushButton(QTStr("Transition"));
	transitionButton->setObjectName("transitionButton");
	transitionButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	QHBoxLayout *quickTransitionsLayout = new QHBoxLayout();
	quickTransitionsLayout->setSpacing(2);

	QPushButton *addQuickTransition = new QPushButton();
	addQuickTransition->setProperty("class", "icon-plus");

	QLabel *quickTransitionsLabel = new QLabel(QTStr("QuickTransitions"));
	quickTransitionsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	quickTransitionsLayout->addWidget(quickTransitionsLabel);
	quickTransitionsLayout->addWidget(addQuickTransition);

	mainButtonLayout->addWidget(transitionButton);
	mainButtonLayout->addWidget(configTransitions);

	tBar = new SliderIgnoreClick(Qt::Horizontal);
	tBar->setMinimum(0);
	tBar->setMaximum(T_BAR_PRECISION - 1);

	tBar->setProperty("class", "slider-tbar");

	connect(tBar, &QSlider::valueChanged, this, &OBSBasic::TBarChanged);
	connect(tBar, &QSlider::sliderReleased, this, &OBSBasic::TBarReleased);

	layout->addStretch(0);
	layout->addLayout(mainButtonLayout);
	layout->addLayout(quickTransitionsLayout);
	layout->addWidget(tBar);
	layout->addStretch(0);

	programOptions->setLayout(layout);

	auto onAdd = [this]() {
		QScopedPointer<QMenu> menu(CreateTransitionMenu(this, nullptr));
		menu->exec(QCursor::pos());
	};

	auto onConfig = [this]() {
		QMenu menu(this);
		QAction *action;

		auto toggleEditProperties = [this]() {
			editPropertiesMode = !editPropertiesMode;

			OBSSource actualScene = OBSGetStrongRef(programScene);
			if (actualScene) {
				TransitionToScene(actualScene, true);
			}
		};

		auto toggleSwapScenesMode = [this]() {
			swapScenesMode = !swapScenesMode;
		};

		auto toggleSceneDuplication = [this]() {
			sceneDuplicationMode = !sceneDuplicationMode;

			OBSSource actualScene = OBSGetStrongRef(programScene);
			if (actualScene) {
				TransitionToScene(actualScene, true);
			}
		};

		auto showToolTip = [&]() {
			QAction *act = menu.activeAction();
			QToolTip::showText(QCursor::pos(), act->toolTip(), &menu, menu.actionGeometry(act));
		};

		action = menu.addAction(QTStr("QuickTransitions.DuplicateScene"));
		action->setToolTip(QTStr("QuickTransitions.DuplicateSceneTT"));
		action->setCheckable(true);
		action->setChecked(sceneDuplicationMode);
		connect(action, &QAction::triggered, this, toggleSceneDuplication);
		connect(action, &QAction::hovered, action, showToolTip);

		action = menu.addAction(QTStr("QuickTransitions.EditProperties"));
		action->setToolTip(QTStr("QuickTransitions.EditPropertiesTT"));
		action->setCheckable(true);
		action->setChecked(editPropertiesMode);
		action->setEnabled(sceneDuplicationMode);
		connect(action, &QAction::triggered, this, toggleEditProperties);
		connect(action, &QAction::hovered, action, showToolTip);

		action = menu.addAction(QTStr("QuickTransitions.SwapScenes"));
		action->setToolTip(QTStr("QuickTransitions.SwapScenesTT"));
		action->setCheckable(true);
		action->setChecked(swapScenesMode);
		connect(action, &QAction::triggered, this, toggleSwapScenesMode);
		connect(action, &QAction::hovered, action, showToolTip);

		menu.exec(QCursor::pos());
	};

	connect(transitionButton.data(), &QAbstractButton::clicked, this, &OBSBasic::TransitionClicked);
	connect(addQuickTransition, &QAbstractButton::clicked, this, onAdd);
	connect(configTransitions, &QAbstractButton::clicked, this, onConfig);
}

void OBSBasic::TogglePreviewProgramMode()
{
	SetPreviewProgramMode(!IsPreviewProgramMode());
}

void OBSBasic::SetPreviewProgramMode(bool enabled)
{
	if (IsPreviewProgramMode() == enabled) {
		return;
	}

	os_atomic_set_bool(&previewProgramMode, enabled);
	emit PreviewProgramModeChanged(enabled);

	if (IsPreviewProgramMode()) {
		if (!previewEnabled) {
			EnablePreviewDisplay(true);
		}

		CreateProgramDisplay();
		CreateProgramOptions();

		OBSScene curScene = GetCurrentScene();

		OBSSceneAutoRelease dup;
		if (sceneDuplicationMode) {
			dup = obs_scene_duplicate(curScene, obs_source_get_name(obs_scene_get_source(curScene)),
						  editPropertiesMode ? OBS_SCENE_DUP_PRIVATE_COPY
								     : OBS_SCENE_DUP_PRIVATE_REFS);
		} else {
			dup = std::move(OBSScene(curScene));
		}

		OBSSourceAutoRelease transition = obs_get_output_source(0);
		obs_source_t *dup_source = obs_scene_get_source(dup);
		obs_transition_set(transition, dup_source);

		if (curScene) {
			obs_source_t *source = obs_scene_get_source(curScene);
			obs_source_inc_showing(source);
			lastScene = OBSGetWeakRef(source);
			programScene = OBSGetWeakRef(source);
		}

		RefreshQuickTransitions();

		programLabel = new QLabel(QTStr("StudioMode.ProgramSceneLabel"), this);
		programLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		programLabel->setProperty("class", "label-preview-title");
		programLabel->setObjectName("programLabel");

		programWidget = new QWidget();
		programLayout = new QVBoxLayout();
		programLayout->setContentsMargins(0, 0, 0, 0);
		programLayout->setSpacing(0);

		programLayout->addWidget(programLabel);

		/* Wirecast puts a snapshot button under the Live monitor */
		QToolButton *snapshotButton = new QToolButton();
		snapshotButton->setObjectName("wirecastSnapshotButton");
		snapshotButton->setIcon(WirecastIcons::icon(WirecastIcons::Glyph::Snapshot, 18));
		snapshotButton->setIconSize(QSize(20, 20));
		snapshotButton->setAutoRaise(true);
		snapshotButton->setToolTip(QTStr("Screenshot.StudioProgram"));
		connect(snapshotButton, &QToolButton::clicked, this, &OBSBasic::ScreenshotProgram);

		programLayout->addWidget(new LiveMonitorFrame(program, snapshotButton));

		programWidget->setLayout(programLayout);

		ui->previewLayout->addWidget(programOptions);

		/* Wirecast keeps transitions under the monitors; only a grip sits between them */
		programOptions->setVisible(false);
		ui->previewLayout->addWidget(programWidget);
		MonitorSplitGrip *grip =
			new MonitorSplitGrip(ui->previewLayout, ui->previewContainer, programWidget, this);
		monitorGrip = grip;
		ui->previewLayout->insertWidget(ui->previewLayout->indexOf(programOptions), grip);

		/* Keep the Live meter on the far right, after the Live monitor */
		if (programMeter) {
			ui->previewLayout->removeWidget(programMeter);
			ui->previewLayout->addWidget(programMeter);
		}
		ui->previewLayout->setAlignment(programOptions, Qt::AlignCenter);

		/* The split grip sizes the monitors, so OBS's keep-them-equal observer is not used */

		OnEvent(OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED);

		blog(LOG_INFO, "Switched to Preview/Program mode");
		blog(LOG_INFO, "-----------------------------"
			       "-------------------");
	} else {
		OBSSource actualProgramScene = OBSGetStrongRef(programScene);
		if (!actualProgramScene) {
			actualProgramScene = GetCurrentSceneSource();
		} else {
			SetCurrentScene(actualProgramScene, true);
		}
		TransitionToScene(actualProgramScene, true);

		delete programOptions;
		delete monitorGrip;
		delete program;
		delete programLabel;
		delete programWidget;
		if (sizeObserver) {
			sizeObserver->deleteLater();
		}

		if (lastScene) {
			OBSSource actualLastScene = OBSGetStrongRef(lastScene);
			if (actualLastScene) {
				obs_source_dec_showing(actualLastScene);
			}
			lastScene = nullptr;
		}

		programScene = nullptr;
		swapScene = nullptr;
		prevFTBSource = nullptr;

		for (QuickTransition &qt : quickTransitions) {
			qt.button = nullptr;
		}

		if (!previewEnabled) {
			EnablePreviewDisplay(false);
		}

		ui->transitions->setEnabled(true);
		tBarActive = false;

		OnEvent(OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED);

		blog(LOG_INFO, "Switched to regular Preview mode");
		blog(LOG_INFO, "-----------------------------"
			       "-------------------");
	}

	ResetUI();
	UpdateTitleBar();
}

void OBSBasic::RenderProgram(void *data, uint32_t, uint32_t)
{
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "RenderProgram");

	OBSBasic *window = static_cast<OBSBasic *>(data);
	obs_video_info ovi;

	obs_get_video_info(&ovi);

	window->programCX = int(window->programScale * float(ovi.base_width));
	window->programCY = int(window->programScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
	gs_set_viewport(window->programX, window->programY, window->programCX, window->programCY);

	obs_render_main_texture_src_color_only();
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	uint32_t width, height;
	obs_display_size(window->program->GetDisplay(), &width, &height);
	gs_ortho(-window->programX, float(width) - window->programX, -window->programY,
		 float(height) - window->programY, -100.0f, 100.0f);
	gs_reset_viewport();
	DrawMonitorBorder(float(window->programCX), float(window->programCY),
			  float(window->program->devicePixelRatioF()), kMonitorBorderLive);

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	GS_DEBUG_MARKER_END();
}

void OBSBasic::ResizeProgram(uint32_t cx, uint32_t cy)
{
	QSize targetSize;

	/* resize program panel to fix to the top section of the window */
	targetSize = GetPixelSize(program);
	GetScaleAndCenterPos(int(cx), int(cy), targetSize.width() - PREVIEW_EDGE_SIZE * 2,
			     targetSize.height() - PREVIEW_EDGE_SIZE * 2, programX, programY, programScale);

	programX += float(PREVIEW_EDGE_SIZE);
	programY += float(PREVIEW_EDGE_SIZE);
}

void OBSBasic::UpdatePreviewProgramIndicators()
{
	bool labels = previewProgramMode ? config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioModeLabels")
					 : false;

	ui->previewLabel->setVisible(labels);

	if (programLabel) {
		programLabel->setVisible(labels);
	}

	if (!labels) {
		return;
	}

	QString preview =
		QTStr("StudioMode.PreviewSceneName").arg(QT_UTF8(obs_source_get_name(GetCurrentSceneSource())));

	QString program = QTStr("StudioMode.ProgramSceneName").arg(QT_UTF8(obs_source_get_name(GetProgramSource())));

	if (ui->previewLabel->text() != preview) {
		ui->previewLabel->setText(preview);
	}

	if (programLabel && programLabel->text() != program) {
		programLabel->setText(program);
	}
}

OBSSource OBSBasic::GetProgramSource()
{
	return OBSGetStrongRef(programScene);
}

void OBSBasic::ProgramViewContextMenuRequested()
{
	QMenu popup(this);
	QPointer<QMenu> studioProgramProjector;

	studioProgramProjector = new QMenu(QTStr("Projector.Open.Program"));
	AddProjectorMenuMonitors(studioProgramProjector, this, &OBSBasic::OpenStudioProgramProjector);
	studioProgramProjector->addSeparator();
	studioProgramProjector->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenStudioProgramWindow);

	popup.addMenu(studioProgramProjector);

	popup.addSeparator();
	popup.addAction(QTStr("Screenshot.StudioProgram"), this, &OBSBasic::ScreenshotProgram);

	popup.exec(QCursor::pos());
}

void OBSBasic::EnablePreviewProgram()
{
	SetPreviewProgramMode(true);
}

void OBSBasic::DisablePreviewProgram()
{
	SetPreviewProgramMode(false);
}

void OBSBasic::OpenStudioProgramProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(nullptr, monitor, ProjectorType::StudioProgram);
}

void OBSBasic::OpenStudioProgramWindow()
{
	OpenProjector(nullptr, -1, ProjectorType::StudioProgram);
}
