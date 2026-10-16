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

/* The documents window, from File > Documents (and at startup only when
 * "Show when LiveMix starts" is ticked). A document is a scene collection.
 *
 *   +---------------------------------------------------------------+
 *   |  [logo] Documents                                         [x] |
 *   +------------------+--------------------------------------------+
 *   | [New document]   |  Recent                                    |
 *   | [Open file...]   |  [thumb]    [thumb]    [thumb]             |
 *   |                  |  Untitled   123        ...                 |
 *   | [ ] Show when    |                                            |
 *   |     LiveMix starts                                            |
 *   +------------------+--------------------------------------------+
 *   |                                        [Close] [Open Untitled]|
 *   +---------------------------------------------------------------+
 *
 * Click a document to select it, double-click (or Open) to open it,
 * right-click or "..." for rename, duplicate, show in folder and delete. */

#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class OBSBasic;
class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
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
	QCheckBox *showOnStart = nullptr;
	QPushButton *openButton = nullptr;
	std::string selected;
	std::map<std::string, QFrame *> cards;

	std::vector<Document> documents() const;
	void rebuild();
	void selectDocument(const std::string &name);

	void openDocument(const std::string &name);
	void createEmpty();
	void openExisting();
	void showDocumentMenu(const Document &document, const QPoint &globalPos);

protected:
	void done(int result) override;
};
