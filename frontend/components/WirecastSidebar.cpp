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

#include "WirecastSidebar.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_WirecastSidebar.cpp"

using WirecastIcons::Glyph;

namespace {
constexpr int kTabIconSize = 26;
} // namespace

WirecastSidebar::WirecastSidebar(QWidget *parent) : QFrame(parent)
{
	setObjectName("wirecastSidebar");

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/* Icon tab row */
	QFrame *tabRow = new QFrame(this);
	tabRow->setObjectName("wirecastSidebarTabs");
	tabLayout = new QHBoxLayout(tabRow);
	tabLayout->setContentsMargins(0, 0, 4, 0);
	tabLayout->setSpacing(0);

	tabs = new QButtonGroup(this);
	tabs->setExclusive(true);

	collapseButton = new QToolButton(tabRow);
	collapseButton->setIcon(WirecastIcons::icon(Glyph::Collapse, 18));
	collapseButton->setIconSize(QSize(18, 18));
	collapseButton->setToolTip(QTStr("Wirecast.HideSidebar"));
	setClasses(collapseButton, "wc-sidebar-collapse");
	connect(collapseButton, &QToolButton::clicked, this, &WirecastSidebar::collapseRequested);

	tabLayout->addStretch();
	tabLayout->addWidget(collapseButton);
	layout->addWidget(tabRow);

	/* Page heading, e.g. "Shot Layers" */
	heading = new QLabel(this);
	heading->setObjectName("wirecastSidebarHeading");
	layout->addWidget(heading);

	stack = new QStackedWidget(this);
	stack->setObjectName("wirecastSidebarPages");
	layout->addWidget(stack, 1);

	connect(tabs, &QButtonGroup::idClicked, this, &WirecastSidebar::setCurrentPage);
}

int WirecastSidebar::addPage(Glyph glyph, const QString &title, QWidget *content)
{
	const int index = int(titles.size());
	titles.push_back(title);

	QToolButton *tab = new QToolButton(this);
	tab->setIcon(WirecastIcons::icon(glyph, kTabIconSize));
	tab->setIconSize(QSize(kTabIconSize, kTabIconSize));
	tab->setCheckable(true);
	tab->setToolTip(title);
	setClasses(tab, "wc-sidebar-tab");
	tabs->addButton(tab, index);

	/* Tabs go before the stretch and the collapse button */
	tabLayout->insertWidget(index, tab);

	stack->addWidget(content);
	content->show();

	if (index == 0) {
		setCurrentPage(0);
	}

	return index;
}

void WirecastSidebar::setPageTitle(int index, const QString &title)
{
	if (index < 0 || index >= int(titles.size())) {
		return;
	}

	titles[index] = title;
	if (QAbstractButton *tab = tabs->button(index)) {
		tab->setToolTip(title);
	}
	if (stack->currentIndex() == index) {
		heading->setText(title);
	}
}

void WirecastSidebar::setCurrentPage(int index)
{
	if (index < 0 || index >= int(titles.size())) {
		return;
	}

	if (QAbstractButton *tab = tabs->button(index)) {
		tab->setChecked(true);
	}

	stack->setCurrentIndex(index);
	heading->setText(titles[index]);
}
