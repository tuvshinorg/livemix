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

/* Wirecast-style left panel: a row of icon tabs, a page heading, and the page
 * contents. It hosts the Shot Editor tabs (see ShotEditor.hpp). */

#pragma once

#include "WirecastIcons.hpp"

#include <QFrame>

#include <vector>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QToolButton;

class WirecastSidebar : public QFrame {
	Q_OBJECT

	std::vector<QString> titles;

	QHBoxLayout *tabLayout = nullptr;
	QButtonGroup *tabs = nullptr;
	QToolButton *collapseButton = nullptr;
	QLabel *heading = nullptr;
	QStackedWidget *stack = nullptr;

public:
	explicit WirecastSidebar(QWidget *parent = nullptr);

	/* Moves content into a new page; it is re-parented to the sidebar */
	int addPage(WirecastIcons::Glyph glyph, const QString &title, QWidget *content);
	void setPageTitle(int index, const QString &title);
	void setCurrentPage(int index);

signals:
	void collapseRequested();
};
