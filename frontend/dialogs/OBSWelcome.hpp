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

/* Wirecast-style welcome window, shown at startup and from Help > Welcome
 * Window. It doubles as the document manager: a document is a scene
 * collection.
 *
 *   +--------------------------------------+------------------------+
 *   |  logo  OBS Studio                    | Create Empty Document  |
 *   |  Version ...                         | Open Existing Document |
 *   |                                      | ---------------------- |
 *   |  Your Documents                      | Recent Documents       |
 *   |  [thumb] [thumb] [thumb] ...         |  [t] Untitled          |
 *   +--------------------------------------+------------------------+
 *   | [ ] Do not show this window again                  [Continue] |
 *   +---------------------------------------------------------------+
 */

#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <vector>

class OBSBasic;
class QCheckBox;
class QLabel;
class QVBoxLayout;
class QWidget;
class FlowLayout;

class OBSWelcome : public QDialog {
	Q_OBJECT

public:
	struct Document {
		std::string name;
		std::filesystem::path file;
		std::filesystem::path thumbnail;
		qint64 modified = 0;
		bool current = false;
	};

	explicit OBSWelcome(OBSBasic *main);

private:
	OBSBasic *main;
	QWidget *gallery = nullptr;
	FlowLayout *galleryLayout = nullptr;
	QLabel *galleryTitle = nullptr;
	QVBoxLayout *recentLayout = nullptr;
	QCheckBox *dontShow = nullptr;

	std::vector<Document> documents() const;
	void rebuild();

	void openDocument(const std::string &name);
	void createEmpty();
	void openExisting();
	void showDocumentMenu(const Document &document, const QPoint &globalPos);

protected:
	void done(int result) override;
};
