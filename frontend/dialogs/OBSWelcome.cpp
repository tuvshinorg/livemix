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
constexpr int kRecentCount = 10;
const QSize kCardThumb(224, 126);
const QSize kRecentThumb(32, 18);

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
	p.fillRect(QRect(QPoint(0, 0), size), QColor(0x0B, 0x0C, 0x0F));

	QPixmap thumbnail(QString::fromStdString(document.thumbnail.u8string()));
	if (!thumbnail.isNull()) {
		p.drawPixmap(QRect(QPoint(0, 0), size), thumbnail);
	} else {
		const int iconSize = std::max(10, size.height() / 3);
		const QPixmap icon = WirecastIcons::pixmap(WirecastIcons::Glyph::Layers, WirecastIcons::kIconMuted,
							   iconSize, ratio);
		p.drawPixmap((size.width() - iconSize) / 2, (size.height() - iconSize) / 2, icon);
	}
	return result;
}

QString modifiedText(qint64 msecs)
{
	if (msecs <= 0) {
		return QString();
	}
	return QLocale().toString(QDateTime::fromMSecsSinceEpoch(msecs), QLocale::ShortFormat);
}

/* Clickable row or card; right-click asks for the document menu */
class DocumentButton : public QFrame {
public:
	std::function<void()> activated;
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

	void contextMenuEvent(QContextMenuEvent *event) override
	{
		if (menuRequested) {
			menuRequested(event->globalPos());
		}
	}
};

QToolButton *actionButton(const QString &iconFile, const QString &text, QWidget *parent)
{
	QToolButton *button = new QToolButton(parent);
	button->setIcon(QIcon(QStringLiteral("theme:Wirecast/%1.svg").arg(iconFile)));
	button->setIconSize(QSize(20, 20));
	button->setText(text);
	button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	button->setCursor(Qt::PointingHandCursor);
	setClasses(button, "wc-welcome-action");
	return button;
}
} // namespace

OBSWelcome::OBSWelcome(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setObjectName("wcWelcome");
	setWindowTitle(QTStr("Wirecast.Welcome.Title"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	resize(1080, 780);
	setMinimumSize(760, 520);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	QHBoxLayout *columns = new QHBoxLayout();
	columns->setContentsMargins(0, 0, 0, 0);
	columns->setSpacing(0);
	layout->addLayout(columns, 1);

	/* ---- Left: start page with every document as a picture ---- */
	QScrollArea *home = new QScrollArea(this);
	home->setObjectName("wcWelcomeHome");
	home->setWidgetResizable(true);
	home->setFrameShape(QFrame::NoFrame);
	home->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QWidget *homeContent = new QWidget(home);
	homeContent->setObjectName("wcWelcomeHomeContent");
	QVBoxLayout *homeLayout = new QVBoxLayout(homeContent);
	homeLayout->setContentsMargins(40, 48, 40, 32);
	homeLayout->setSpacing(0);

	QHBoxLayout *brand = new QHBoxLayout();
	brand->setSpacing(18);
	brand->addStretch();
	QLabel *logo = new QLabel(homeContent);
	logo->setPixmap(QApplication::windowIcon().pixmap(QSize(76, 76), devicePixelRatioF()));
	QLabel *appName = new QLabel(QStringLiteral("OBS Studio"), homeContent);
	appName->setObjectName("wcWelcomeAppName");
	brand->addWidget(logo);
	brand->addWidget(appName);
	brand->addStretch();
	homeLayout->addLayout(brand);

	QLabel *version = new QLabel(QTStr("Wirecast.Welcome.Version").arg(App()->GetVersionString(false).c_str()),
				     homeContent);
	version->setObjectName("wcWelcomeVersion");
	version->setAlignment(Qt::AlignCenter);
	homeLayout->addSpacing(14);
	homeLayout->addWidget(version);
	homeLayout->addSpacing(40);

	galleryTitle = new QLabel(homeContent);
	galleryTitle->setObjectName("wcWelcomeSection");
	homeLayout->addWidget(galleryTitle);
	homeLayout->addSpacing(12);

	gallery = new QWidget(homeContent);
	galleryLayout = new FlowLayout(gallery, 0, 16, 16);
	homeLayout->addWidget(gallery);
	homeLayout->addStretch();

	home->setWidget(homeContent);
	columns->addWidget(home, 1);

	/* ---- Right: actions and recent documents ---- */
	QFrame *side = new QFrame(this);
	side->setObjectName("wcWelcomeSide");
	side->setFixedWidth(268);
	QVBoxLayout *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(10, 10, 10, 10);
	sideLayout->setSpacing(2);

	QToolButton *createButton =
		actionButton(QStringLiteral("file-plus"), QTStr("Wirecast.Welcome.CreateEmpty"), side);
	QToolButton *openButton =
		actionButton(QStringLiteral("folder-open"), QTStr("Wirecast.Welcome.OpenExisting"), side);
	connect(createButton, &QToolButton::clicked, this, &OBSWelcome::createEmpty);
	connect(openButton, &QToolButton::clicked, this, &OBSWelcome::openExisting);
	sideLayout->addWidget(createButton);
	sideLayout->addWidget(openButton);

	QFrame *line = new QFrame(side);
	line->setObjectName("wcWelcomeLine");
	line->setFixedHeight(1);
	sideLayout->addSpacing(6);
	sideLayout->addWidget(line);
	sideLayout->addSpacing(8);

	QLabel *recentTitle = new QLabel(QTStr("Wirecast.Welcome.Recent"), side);
	recentTitle->setObjectName("wcWelcomeRecentTitle");
	sideLayout->addWidget(recentTitle);
	sideLayout->addSpacing(6);

	recentLayout = new QVBoxLayout();
	recentLayout->setContentsMargins(0, 0, 0, 0);
	recentLayout->setSpacing(1);
	sideLayout->addLayout(recentLayout);
	sideLayout->addStretch();
	columns->addWidget(side);

	/* ---- Footer ---- */
	QFrame *footer = new QFrame(this);
	footer->setObjectName("wcWelcomeFooter");
	QHBoxLayout *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(18, 12, 16, 12);
	dontShow = new QCheckBox(QTStr("Wirecast.Welcome.DontShow"), footer);
	dontShow->setChecked(!config_get_bool(App()->GetUserConfig(), "General", "WirecastShowWelcome"));
	QPushButton *continueButton = new QPushButton(QTStr("Wirecast.Welcome.Continue"), footer);
	continueButton->setObjectName("wcWelcomeContinue");
	continueButton->setFixedSize(130, 30);
	continueButton->setDefault(true);
	connect(continueButton, &QPushButton::clicked, this, &QDialog::accept);
	footerLayout->addWidget(dontShow);
	footerLayout->addStretch();
	footerLayout->addWidget(continueButton);
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
	for (QLayout *list : {static_cast<QLayout *>(galleryLayout), static_cast<QLayout *>(recentLayout)}) {
		while (QLayoutItem *child = list->takeAt(0)) {
			if (QWidget *w = child->widget()) {
				w->hide();
				w->deleteLater();
			}
			delete child;
		}
	}

	const std::vector<Document> docs = documents();
	const qreal ratio = devicePixelRatioF();
	galleryTitle->setText(QTStr("Wirecast.Welcome.Documents").arg(docs.size()));

	for (const Document &document : docs) {
		/* Gallery card: picture, name, last saved */
		DocumentButton *card = new DocumentButton(gallery);
		setClasses(card, document.current ? "wc-welcome-card wc-welcome-current" : "wc-welcome-card");
		QVBoxLayout *cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(6, 6, 6, 8);
		cardLayout->setSpacing(4);

		QLabel *picture = new QLabel(card);
		picture->setFixedSize(kCardThumb);
		picture->setPixmap(documentPixmap(document, kCardThumb, 5, ratio));
		cardLayout->addWidget(picture);

		QLabel *name = new QLabel(card);
		name->setObjectName("wcWelcomeCardName");
		name->setText(QFontMetrics(name->font())
				      .elidedText(QT_UTF8(document.name.c_str()), Qt::ElideRight, kCardThumb.width()));
		cardLayout->addWidget(name);

		QLabel *detail = new QLabel(card);
		detail->setObjectName("wcWelcomeCardDetail");
		detail->setText(document.current ? QTStr("Wirecast.Welcome.OpenNow")
						 : modifiedText(document.modified));
		cardLayout->addWidget(detail);

		card->setToolTip(QT_UTF8(document.name.c_str()));
		const std::string docName = document.name;
		card->activated = [this, docName]() { openDocument(docName); };
		card->menuRequested = [this, document](const QPoint &pos) { showDocumentMenu(document, pos); };
		galleryLayout->addWidget(card);
	}

	/* Recent Documents list on the right */
	int count = 0;
	for (const Document &document : docs) {
		if (count++ >= kRecentCount) {
			break;
		}

		DocumentButton *row = new DocumentButton(this);
		setClasses(row, "wc-welcome-recent");
		QHBoxLayout *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(8, 4, 8, 4);
		rowLayout->setSpacing(10);

		QLabel *picture = new QLabel(row);
		picture->setFixedSize(kRecentThumb);
		picture->setPixmap(documentPixmap(document, kRecentThumb, 2, ratio));
		QLabel *name = new QLabel(QT_UTF8(document.name.c_str()), row);
		name->setObjectName("wcWelcomeRecentName");
		rowLayout->addWidget(picture);
		rowLayout->addWidget(name, 1);

		row->setToolTip(QString::fromStdString(document.file.u8string()));
		const std::string docName = document.name;
		row->activated = [this, docName]() { openDocument(docName); };
		row->menuRequested = [this, document](const QPoint &pos) { showDocumentMenu(document, pos); };
		recentLayout->addWidget(row);
	}
}

void OBSWelcome::openDocument(const std::string &name)
{
	if (main->OpenCollection(name)) {
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

	const QString file = OpenFile(this, QTStr("Wirecast.Welcome.OpenExisting"),
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
		main->RenameCollection(name, newName);
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
	config_set_bool(App()->GetUserConfig(), "General", "WirecastShowWelcome", !dontShow->isChecked());
	QDialog::done(result);
}
