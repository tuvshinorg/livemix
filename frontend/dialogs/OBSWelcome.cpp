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

#include "OBSWelcome.hpp"

#include <OBSApp.hpp>
#include <components/FlowLayout.hpp>
#include <components/WirecastIcons.hpp>
#include <dialogs/NameDialog.hpp>
#include <models/SceneCollection.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "moc_OBSWelcome.cpp"

namespace {
const QSize kCardThumb(224, 126);
constexpr int kCardRadius = 8;
constexpr const char *kShowAtStartKey = "LiveMixShowDocuments";

/* The document's thumbnail, rounded; an empty dark card with the layers icon
 * when it has none yet */
QPixmap documentPixmap(const OBSWelcome::Document &document, QSize size, int radius, qreal ratio)
{
	QPixmap result(size * ratio);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);

	QPainter p(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	QPainterPath clip;
	clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), radius, radius);
	p.setClipPath(clip);
	p.fillRect(QRect(QPoint(0, 0), size), QColor(0x15, 0x16, 0x18));

	QPixmap thumbnail(QString::fromStdString(document.thumbnail.u8string()));
	if (!thumbnail.isNull()) {
		p.drawPixmap(QRect(QPoint(0, 0), size), thumbnail);
	} else {
		const int iconSize = std::max(10, size.height() / 4);
		const QPixmap icon = WirecastIcons::pixmap(WirecastIcons::Glyph::Layers, WirecastIcons::kIconMuted,
							   iconSize, ratio);
		p.drawPixmap((size.width() - iconSize) / 2, (size.height() - iconSize) / 2, icon);
	}

	/* Hairline outline, so dark thumbnails still read as cards */
	p.setClipping(false);
	p.setPen(QPen(QColor(0x2C, 0x2E, 0x33), 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1, size.height() - 1), radius, radius);
	return result;
}

QString modifiedText(qint64 msecs)
{
	if (msecs <= 0) {
		return QString();
	}
	const QDateTime when = QDateTime::fromMSecsSinceEpoch(msecs);
	const qint64 days = when.date().daysTo(QDate::currentDate());
	if (days == 0) {
		return QTStr("Wirecast.Documents.EditedToday").arg(QLocale().toString(when.time(), QLocale::ShortFormat));
	}
	if (days == 1) {
		return QTStr("Wirecast.Documents.EditedYesterday");
	}
	return QTStr("Wirecast.Documents.Edited").arg(QLocale().toString(when.date(), QLocale::ShortFormat));
}

/* Clickable card; double-click opens, right-click asks for the document menu */
class DocumentButton : public QFrame {
public:
	std::function<void()> activated;
	std::function<void()> doubleActivated;
	std::function<void(const QPoint &)> menuRequested;

	explicit DocumentButton(QWidget *parent) : QFrame(parent)
	{
		setCursor(Qt::PointingHandCursor);
		setAttribute(Qt::WA_Hover);
	}

protected:
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && activated) {
			activated();
			return;
		}
		QFrame::mouseReleaseEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && doubleActivated) {
			doubleActivated();
			return;
		}
		QFrame::mouseDoubleClickEvent(event);
	}

	void contextMenuEvent(QContextMenuEvent *event) override
	{
		if (menuRequested) {
			menuRequested(event->globalPos());
		}
	}
};

QPushButton *pushButton(const QString &text, const char *classes, QWidget *parent)
{
	QPushButton *button = new QPushButton(text, parent);
	button->setFixedHeight(36);
	button->setCursor(Qt::PointingHandCursor);
	setClasses(button, classes);
	return button;
}
} // namespace

OBSWelcome::OBSWelcome(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setObjectName("lmDocuments");
	setWindowTitle(QTStr("Wirecast.Documents.Title"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	resize(1000, 640);
	setMinimumSize(720, 480);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/* ---- Header ---- */
	QFrame *header = new QFrame(this);
	header->setObjectName("lmDocumentsHeader");
	QHBoxLayout *headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(24, 16, 24, 16);
	headerLayout->setSpacing(12);
	QLabel *logo = new QLabel(header);
	logo->setPixmap(QApplication::windowIcon().pixmap(QSize(22, 22), devicePixelRatioF()));
	QLabel *title = new QLabel(QTStr("Wirecast.Documents.Title"), header);
	title->setObjectName("lmDocumentsTitle");
	headerLayout->addWidget(logo);
	headerLayout->addWidget(title);
	headerLayout->addStretch();
	layout->addWidget(header);

	QHBoxLayout *columns = new QHBoxLayout();
	columns->setContentsMargins(0, 0, 0, 0);
	columns->setSpacing(0);
	layout->addLayout(columns, 1);

	/* ---- Left: create / open, and the startup setting ---- */
	QFrame *side = new QFrame(this);
	side->setObjectName("lmDocumentsSide");
	side->setFixedWidth(248);
	QVBoxLayout *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(20, 20, 20, 20);
	sideLayout->setSpacing(8);

	QPushButton *createButton = pushButton(QTStr("Wirecast.Documents.New"), "lm-primary-button", side);
	QPushButton *openFileButton = pushButton(QTStr("Wirecast.Documents.OpenFile"), "lm-secondary-button", side);
	createButton->setAutoDefault(false);
	openFileButton->setAutoDefault(false);
	connect(createButton, &QPushButton::clicked, this, &OBSWelcome::createEmpty);
	connect(openFileButton, &QPushButton::clicked, this, &OBSWelcome::openExisting);
	sideLayout->addWidget(createButton);
	sideLayout->addWidget(openFileButton);
	sideLayout->addStretch();

	showOnStart = new QCheckBox(QTStr("Wirecast.Documents.ShowOnStart"), side);
	showOnStart->setChecked(config_get_bool(App()->GetUserConfig(), "General", kShowAtStartKey));
	QLabel *hint = new QLabel(QTStr("Wirecast.Documents.ShowOnStart.Hint"), side);
	hint->setObjectName("lmDocumentsHint");
	hint->setWordWrap(true);
	sideLayout->addWidget(showOnStart);
	sideLayout->addWidget(hint);
	columns->addWidget(side);

	/* ---- Right: every document as a card ---- */
	QScrollArea *body = new QScrollArea(this);
	body->setObjectName("lmDocumentsBody");
	body->setWidgetResizable(true);
	body->setFrameShape(QFrame::NoFrame);
	body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QWidget *bodyContent = new QWidget(body);
	bodyContent->setObjectName("lmDocumentsBodyContent");
	QVBoxLayout *bodyLayout = new QVBoxLayout(bodyContent);
	bodyLayout->setContentsMargins(24, 20, 24, 24);
	bodyLayout->setSpacing(14);

	QLabel *section = new QLabel(QTStr("Wirecast.Documents.Recent"), bodyContent);
	section->setObjectName("lmDocumentsSection");
	bodyLayout->addWidget(section);

	gallery = new QWidget(bodyContent);
	galleryLayout = new FlowLayout(gallery, 0, 16, 16);
	bodyLayout->addWidget(gallery);
	bodyLayout->addStretch();

	body->setWidget(bodyContent);
	columns->addWidget(body, 1);

	/* ---- Footer ---- */
	QFrame *footer = new QFrame(this);
	footer->setObjectName("lmDocumentsFooter");
	QHBoxLayout *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(16, 14, 16, 14);
	footerLayout->setSpacing(8);
	QPushButton *closeButton = pushButton(QTStr("Close"), "lm-secondary-button", footer);
	closeButton->setAutoDefault(false);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
	openButton = pushButton(QString(), "lm-primary-button", footer);
	openButton->setDefault(true);
	connect(openButton, &QPushButton::clicked, this, [this]() { openDocument(selected); });
	footerLayout->addStretch();
	footerLayout->addWidget(closeButton);
	footerLayout->addWidget(openButton);
	layout->addWidget(footer);

	rebuild();
}

std::vector<OBSWelcome::Document> OBSWelcome::documents() const
{
	const std::string current = config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection");

	std::vector<Document> result;
	for (const auto &[name, collection] : main->GetSceneCollectionCache()) {
		Document document;
		document.name = name;
		document.file = collection.getFilePath();
		document.thumbnail = main->CollectionThumbnailPath(collection.getFileStem());
		document.modified =
			QFileInfo(QString::fromStdString(document.file.u8string())).lastModified().toMSecsSinceEpoch();
		document.current = name == current;
		result.push_back(std::move(document));
	}

	/* Most recently used first */
	std::sort(result.begin(), result.end(),
		  [](const Document &a, const Document &b) { return a.modified > b.modified; });
	return result;
}

void OBSWelcome::rebuild()
{
	while (QLayoutItem *child = galleryLayout->takeAt(0)) {
		if (QWidget *w = child->widget()) {
			w->hide();
			w->deleteLater();
		}
		delete child;
	}
	cards.clear();

	const std::vector<Document> docs = documents();
	const qreal ratio = devicePixelRatioF();

	/* Keep the selection if that document still exists, else select the open one */
	const bool keep = std::any_of(docs.begin(), docs.end(), [this](const Document &d) { return d.name == selected; });
	if (!keep) {
		selected.clear();
		for (const Document &document : docs) {
			if (document.current || selected.empty()) {
				selected = document.name;
			}
		}
	}

	for (const Document &document : docs) {
		/* Card: picture, then name and "Open now" / last edited, with a "..." menu button */
		DocumentButton *card = new DocumentButton(gallery);
		QVBoxLayout *cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(4, 4, 4, 6);
		cardLayout->setSpacing(8);

		QLabel *picture = new QLabel(card);
		picture->setFixedSize(kCardThumb);
		picture->setPixmap(documentPixmap(document, kCardThumb, kCardRadius, ratio));
		cardLayout->addWidget(picture);

		QHBoxLayout *caption = new QHBoxLayout();
		caption->setContentsMargins(2, 0, 0, 0);
		caption->setSpacing(4);
		QVBoxLayout *text = new QVBoxLayout();
		text->setContentsMargins(0, 0, 0, 0);
		text->setSpacing(2);

		QLabel *name = new QLabel(card);
		name->setObjectName("lmDocCardName");
		name->setText(QFontMetrics(name->font())
				      .elidedText(QT_UTF8(document.name.c_str()), Qt::ElideRight, kCardThumb.width() - 40));
		QLabel *detail = new QLabel(card);
		detail->setObjectName("lmDocCardDetail");
		detail->setText(document.current ? QTStr("Wirecast.Welcome.OpenNow") : modifiedText(document.modified));
		text->addWidget(name);
		text->addWidget(detail);
		caption->addLayout(text, 1);

		QToolButton *more = new QToolButton(card);
		more->setIcon(QIcon(QStringLiteral("theme:Wirecast/dots.svg")));
		more->setIconSize(QSize(16, 16));
		more->setFixedSize(30, 30);
		more->setToolTip(QTStr("Wirecast.Documents.More").arg(QT_UTF8(document.name.c_str())));
		setClasses(more, "lm-doc-more");
		caption->addWidget(more, 0, Qt::AlignVCenter);
		cardLayout->addLayout(caption);

		card->setToolTip(QString::fromStdString(document.file.u8string()));
		const std::string docName = document.name;
		card->activated = [this, docName]() { selectDocument(docName); };
		card->doubleActivated = [this, docName]() { openDocument(docName); };
		card->menuRequested = [this, document](const QPoint &pos) { showDocumentMenu(document, pos); };
		connect(more, &QToolButton::clicked, this, [this, document, more]() {
			showDocumentMenu(document, more->mapToGlobal(QPoint(0, more->height())));
		});

		cards[document.name] = card;
		galleryLayout->addWidget(card);
	}

	selectDocument(selected);
}

void OBSWelcome::selectDocument(const std::string &name)
{
	selected = name;
	for (const auto &[cardName, card] : cards) {
		setClasses(card, cardName == name ? "lm-doc-card lm-doc-selected" : "lm-doc-card");
	}

	openButton->setEnabled(!name.empty());
	openButton->setText(name.empty() ? QTStr("Wirecast.Welcome.Open")
					 : QTStr("Wirecast.Documents.OpenNamed").arg(QT_UTF8(name.c_str())));
}

void OBSWelcome::openDocument(const std::string &name)
{
	if (!name.empty() && main->OpenCollection(name)) {
		accept();
	}
}

void OBSWelcome::createEmpty()
{
	if (main->CreateUntitledCollection()) {
		accept();
	}
}

void OBSWelcome::openExisting()
{
	std::filesystem::path folder;
	const std::vector<Document> docs = documents();
	if (!docs.empty()) {
		folder = docs.front().file.parent_path();
	}

	const QString file = OpenFile(this, QTStr("Wirecast.Documents.OpenFile"),
				      QString::fromStdString(folder.u8string()),
				      QTStr("Wirecast.Welcome.FileFilter") + QStringLiteral(" (*.json)"));
	if (file.isEmpty()) {
		return;
	}
	if (main->OpenCollectionFile(file)) {
		accept();
	}
}

void OBSWelcome::showDocumentMenu(const Document &document, const QPoint &globalPos)
{
	const std::string name = document.name;
	const QString qname = QT_UTF8(name.c_str());

	selectDocument(name);

	QMenu menu(this);
	connect(menu.addAction(QTStr("Wirecast.Welcome.Open")), &QAction::triggered, this,
		[this, name]() { openDocument(name); });
	connect(menu.addAction(QTStr("Rename")), &QAction::triggered, this, [this, name, qname]() {
		std::string newName;
		if (!NameDialog::AskForName(this, QTStr("Wirecast.Welcome.Rename.Title"),
					    QTStr("Wirecast.Welcome.Rename.Text"), newName, qname)) {
			return;
		}
		if (newName.empty() || newName == name) {
			return;
		}
		if (main->GetSceneCollectionByName(newName)) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			return;
		}
		if (main->RenameCollection(name, newName)) {
			selected = newName;
		}
		rebuild();
	});
	connect(menu.addAction(QTStr("Wirecast.Welcome.Duplicate")), &QAction::triggered, this, [this, name]() {
		main->DuplicateCollection(name);
		rebuild();
	});
	const QString folder = QString::fromStdString(document.file.parent_path().u8string());
	connect(menu.addAction(QTStr("Wirecast.Welcome.ShowInFolder")), &QAction::triggered, this,
		[folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
	menu.addSeparator();

	QAction *remove = menu.addAction(QTStr("Wirecast.Welcome.Delete"));
	remove->setEnabled(main->GetSceneCollectionCache().size() > 1);
	connect(remove, &QAction::triggered, this, [this, name, qname]() {
		if (OBSMessageBox::question(this, QTStr("Wirecast.Welcome.Delete.Title"),
					    QTStr("Wirecast.Welcome.Delete.Text").arg(qname)) != QMessageBox::Yes) {
			return;
		}
		main->RemoveCollection(name);
		rebuild();
	});

	menu.exec(globalPos);
}

void OBSWelcome::done(int result)
{
	config_set_bool(App()->GetUserConfig(), "General", kShowAtStartKey, showOnStart->isChecked());
	QDialog::done(result);
}
