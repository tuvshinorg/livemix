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

#include "BroadcastBar.hpp"
#include "WirecastIcons.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>

#include "moc_BroadcastBar.cpp"

using WirecastIcons::Glyph;

namespace {
constexpr int kIconSize = 18;
constexpr int kLayoutIconSize = 18;
constexpr QSize kToolButtonSize(34, 34);
constexpr int kOutputButtonHeight = 34;
/* Text and icon of a running output, on its red-tinted pill */
const QColor kActiveOutputColor(0xF6, 0xB3, 0xA5);

QToolButton *createToolButton(QWidget *parent, Glyph glyph, const QString &tooltip)
{
	QToolButton *button = new QToolButton(parent);
	button->setIcon(WirecastIcons::icon(glyph, kIconSize));
	button->setIconSize(QSize(kIconSize, kIconSize));
	button->setFixedSize(kToolButtonSize);
	button->setToolTip(tooltip);
	button->setAutoRaise(true);
	setClasses(button, "wc-toolbar-button");
	return button;
}

QString formatElapsed(qint64 ms)
{
	const qint64 total = ms / 1000;
	return QString("%1:%2:%3")
		.arg(total / 3600, 2, 10, QChar('0'))
		.arg((total / 60) % 60, 2, 10, QChar('0'))
		.arg(total % 60, 2, 10, QChar('0'));
}
} // namespace

BroadcastBar::BroadcastBar(QWidget *parent) : QFrame(parent)
{
	setObjectName("wirecastBroadcastBar");

	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(10, 5, 10, 5);
	layout->setSpacing(8);

	expandButton = createToolButton(this, Glyph::Expand, QTStr("Wirecast.ShowSidebar"));
	expandButton->setVisible(false);
	connect(expandButton, &QToolButton::clicked, this, &BroadcastBar::expandSidebarClicked);
	layout->addWidget(expandButton);

	setupClock(stream, int(Glyph::Broadcast), QTStr("Wirecast.Stream"), QTStr("Basic.Main.StartStreaming"));
	setupClock(record, int(Glyph::Record), QTStr("Wirecast.Record"), QTStr("Basic.Main.StartRecording"));
	setupClock(replay, int(Glyph::Iso), QTStr("Wirecast.Replay"), QTStr("Basic.Main.StartReplayBuffer"));

	for (OutputClock *clock : {&stream, &record, &replay}) {
		layout->addWidget(clock->button);
	}

	connect(stream.button, &QToolButton::clicked, this, &BroadcastBar::streamClicked);
	connect(record.button, &QToolButton::clicked, this, &BroadcastBar::recordClicked);
	connect(replay.button, &QToolButton::clicked, this, &BroadcastBar::replayClicked);

	virtualCamButton = createToolButton(this, Glyph::Display, QTStr("Basic.Main.StartVirtualCam"));
	connect(virtualCamButton, &QToolButton::clicked, this, &BroadcastBar::virtualCamClicked);
	layout->addWidget(virtualCamButton);

	layout->addStretch();

	/* Monitor layout switch, like Wirecast's top-right layout buttons */
	QFrame *layoutGroup = new QFrame(this);
	layoutGroup->setObjectName("wirecastLayoutGroup");
	QHBoxLayout *groupLayout = new QHBoxLayout(layoutGroup);
	groupLayout->setContentsMargins(3, 3, 3, 3);
	groupLayout->setSpacing(2);

	auto createLayoutButton = [&](Glyph glyph, const QString &tooltip, int mode) {
		QToolButton *button = new QToolButton(layoutGroup);
		button->setIcon(WirecastIcons::icon(glyph, kLayoutIconSize));
		button->setIconSize(QSize(kLayoutIconSize, kLayoutIconSize));
		button->setFixedSize(34, 28);
		button->setCheckable(true);
		button->setToolTip(tooltip);
		setClasses(button, "wc-layout-button");
		groupLayout->addWidget(button);
		connect(button, &QToolButton::clicked, this, [this, mode]() { emit monitorModeRequested(mode); });
		return button;
	};

	dualButton = createLayoutButton(Glyph::LayoutDual, QTStr("Wirecast.Layout.Dual"), LiveAndPreview);
	previewButton = createLayoutButton(Glyph::LayoutPreview, QTStr("Wirecast.Layout.Preview"), PreviewOnly);
	liveButton = createLayoutButton(Glyph::LayoutLive, QTStr("Wirecast.Layout.Live"), LiveOnly);

	QButtonGroup *layoutButtons = new QButtonGroup(this);
	layoutButtons->setExclusive(true);
	layoutButtons->addButton(dualButton);
	layoutButtons->addButton(previewButton);
	layoutButtons->addButton(liveButton);
	layout->addWidget(layoutGroup);

	setMonitorMode(LiveOnly);

	clockTimer.setInterval(500);
	connect(&clockTimer, &QTimer::timeout, this, &BroadcastBar::updateClocks);

	obs_frontend_add_event_callback(&BroadcastBar::onFrontendEvent, this);
	eventCallbackRegistered = true;
}

BroadcastBar::~BroadcastBar()
{
	if (eventCallbackRegistered) {
		obs_frontend_remove_event_callback(&BroadcastBar::onFrontendEvent, this);
	}
}

void BroadcastBar::setupClock(OutputClock &clock, int glyph, const QString &caption, const QString &tooltip)
{
	clock.caption = caption;
	clock.glyph = glyph;

	QToolButton *button = new QToolButton(this);
	button->setIcon(WirecastIcons::icon(Glyph(glyph), kIconSize, WirecastIcons::kIconColor));
	button->setIconSize(QSize(kIconSize, kIconSize));
	button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	button->setFixedHeight(kOutputButtonHeight);
	button->setToolTip(tooltip);
	button->setCursor(Qt::PointingHandCursor);
	setClasses(button, "wc-output-button");

	/* Same-width digits, and room for the longest clock, so the pill never jumps as time runs */
	QFont font = button->font();
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	font.setFeature(QFont::Tag("tnum"), 1);
#endif
	button->setFont(font);
	clock.button = button;
	updateClockLabel(clock);
	button->setMinimumWidth(button->sizeHint().width() + 6);
}

void BroadcastBar::setClockActive(OutputClock &clock, bool active)
{
	clock.active = active;
	clock.button->setIcon(WirecastIcons::icon(Glyph(clock.glyph), kIconSize,
						  active ? kActiveOutputColor : WirecastIcons::kIconColor));
	if (active) {
		clock.elapsed.start();
	}
	setClasses(clock.button, active ? "wc-output-button wc-output-active" : "wc-output-button");
	updateClockLabel(clock);

	if (stream.active || record.active || replay.active) {
		clockTimer.start();
	} else {
		clockTimer.stop();
	}
}

void BroadcastBar::updateClockLabel(OutputClock &clock)
{
	const qint64 ms = clock.active ? clock.elapsed.elapsed() : 0;
	clock.button->setText(QString("%1   %2").arg(clock.caption, formatElapsed(ms)));
}

void BroadcastBar::updateClocks()
{
	updateClockLabel(stream);
	updateClockLabel(record);
	updateClockLabel(replay);
}

void BroadcastBar::setVirtualCamActive(bool active)
{
	virtualCamButton->setIcon(WirecastIcons::icon(
		Glyph::Display, kIconSize, active ? WirecastIcons::kRed : WirecastIcons::kIconMuted));
	virtualCamButton->setToolTip(active ? QTStr("Basic.Main.StopVirtualCam")
					    : QTStr("Basic.Main.StartVirtualCam"));
}

void BroadcastBar::setMonitorMode(int mode)
{
	dualButton->setChecked(mode == LiveAndPreview);
	previewButton->setChecked(mode == PreviewOnly);
	liveButton->setChecked(mode == LiveOnly);
}

void BroadcastBar::setSidebarCollapsed(bool collapsed)
{
	expandButton->setVisible(collapsed);
}

void BroadcastBar::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	BroadcastBar *bar = static_cast<BroadcastBar *>(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		bar->setClockActive(bar->stream, true);
		bar->stream.button->setToolTip(QTStr("Basic.Main.StopStreaming"));
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		bar->setClockActive(bar->stream, false);
		bar->stream.button->setToolTip(QTStr("Basic.Main.StartStreaming"));
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		bar->setClockActive(bar->record, true);
		bar->record.button->setToolTip(QTStr("Basic.Main.StopRecording"));
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		bar->setClockActive(bar->record, false);
		bar->record.button->setToolTip(QTStr("Basic.Main.StartRecording"));
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		bar->setClockActive(bar->replay, true);
		bar->replay.button->setToolTip(QTStr("Basic.Main.StopReplayBuffer"));
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		bar->setClockActive(bar->replay, false);
		bar->replay.button->setToolTip(QTStr("Basic.Main.StartReplayBuffer"));
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		bar->setVirtualCamActive(true);
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		bar->setVirtualCamActive(false);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_event_callback(&BroadcastBar::onFrontendEvent, bar);
		bar->eventCallbackRegistered = false;
		break;
	default:
		break;
	}
}
