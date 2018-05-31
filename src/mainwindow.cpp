#include "mainwindow.h"

#include <iostream>
#include <fstream>
#include <map>

#include <QWebEnginePage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonObject>
#include <QTextDocument>
#include <QLineEdit>
#include <QDesktopServices>
#include <QDir>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#include <QKeyEvent>
#include <QMenu>
#include <QStandardItemModel>
#include <QFontDatabase>
#include <QWebEngineHistory>

#include "WebSocketClient.h"
#include "JavascriptWrapper.h"

#include "uploader.h"
#include "check.h"
#include "StopApplication.h"
#include "duration.h"
#include "Log.h"
#include "utils.h"
#include "algorithms.h"

#include "machine_uid.h"

bool EvFilter::eventFilter(QObject * watched, QEvent * event) {
    QToolButton * button = qobject_cast<QToolButton*>(watched);
    if (!button) {
        return false;
    }

    if (event->type() == QEvent::Enter) {
        // The push button is hovered by mouse
        button->setIcon(icoHover);
        return true;
    } else if (event->type() == QEvent::Leave){
        // The push button is not hovered by mouse
        button->setIcon(icoActive);
        return true;
    }

    return false;
}

static QString makeMessageForWss(const QString &hardwareId, const QString &userId, size_t focusCount, const QString &line, bool isEnter) {
    QJsonObject allJson;
    allJson.insert("app", "MetaSearch");
    QJsonObject data;
    data.insert("machine_uid", hardwareId);
    data.insert("user_id", userId);
    data.insert("focus_release_count", (int)focusCount);
    data.insert("text", QString(line.toUtf8().toHex()));
    data.insert("is_enter_pressed", isEnter);
    allJson.insert("data", data);
    QJsonDocument json(allJson);

    return json.toJson(QJsonDocument::Compact);
}

static QString makeMessageApplicationForWss(const QString &hardwareId, const QString &userId, const QString &applicationVersion, const QString &interfaceVersion) {
    QJsonObject allJson;
    allJson.insert("app", "MetaGate");
    QJsonObject data;
    data.insert("machine_uid", hardwareId);
    data.insert("user_id", userId);
    data.insert("application_ver", applicationVersion);
    data.insert("interface_ver", interfaceVersion);
    allJson.insert("data", data);
    QJsonDocument json(allJson);

    return json.toJson(QJsonDocument::Compact);
}

MainWindow::MainWindow(WebSocketClient &webSocketClient, JavascriptWrapper &jsWrapper, const QString &applicationVersion, QWidget *parent)
    : QMainWindow(parent)
    , ui(std::make_unique<Ui::MainWindow>())
    , webSocketClient(webSocketClient)
    , jsWrapper(jsWrapper)
    , applicationVersion(applicationVersion)
{
    ui->setupUi(this);

    hardwareId = QString::fromStdString(::getMachineUid());

    configureMenu();

    currentBeginPath = Uploader::getPagesPath();
    const auto &lastVersionPair = Uploader::getLastVersion(currentBeginPath);
    folderName = lastVersionPair.first;
    lastVersion = lastVersionPair.second;

    hardReloadPage("login.html");
    setCommandLineText2("app://Login", true, true);

    jsWrapper.setWidget(this);

    client.setParent(this);
    CHECK(connect(&client, SIGNAL(callbackCall(ReturnCallback)), this, SLOT(callbackCall(ReturnCallback))), "not connect");

    CHECK(connect(&jsWrapper, SIGNAL(jsRunSig(QString)), this, SLOT(onJsRun(QString))), "not connect");
    CHECK(connect(&jsWrapper, SIGNAL(setHasNativeToolbarVariableSig()), this, SLOT(onSetHasNativeToolbarVariable())), "not connect");
    CHECK(connect(&jsWrapper, SIGNAL(setCommandLineTextSig(QString)), this, SLOT(onSetCommandLineText(QString))), "not connect");
    CHECK(connect(&jsWrapper, SIGNAL(setUserNameSig(QString)), this, SLOT(onSetUserName(QString))), "not connect");
    CHECK(connect(&jsWrapper, SIGNAL(setMappingsSig(QString)), this, SLOT(onSetMappings(QString))), "not connect");
    CHECK(connect(&jsWrapper, SIGNAL(lineEditReturnPressedSig(QString)), this, SLOT(enterCommandAndAddToHistory(QString))), "not connect");

    channel = std::make_unique<QWebChannel>(ui->webView->page());
    ui->webView->page()->setWebChannel(channel.get());
    channel->registerObject(QString("mainWindow"), &jsWrapper);

    ui->webView->setContextMenuPolicy(Qt::CustomContextMenu);
    CHECK(connect(ui->webView, SIGNAL(customContextMenuRequested(const QPoint &)), this, SLOT(ShowContextMenu(const QPoint &))), "not connect");

    CHECK(connect(ui->webView->page(), &QWebEnginePage::loadFinished, this, &MainWindow::browserLoadFinished), "not connect");

    /*CHECK(connect(ui->webView->page(), &QWebEnginePage::urlChanged, [this](const QUrl &url) {
        if (url.toString().startsWith(METAHASH_URL)) {
            LOG << "Url changed!!! " << url.toString();
            lineEditReturnPressed(url.toString());
        }
        LOG << "Url not changed!!! " << url.toString();
    }), "not connect");*/

    qtimer.setInterval(hours(1).count());
    qtimer.setSingleShot(false);
    CHECK(connect(&qtimer, SIGNAL(timeout()), this, SLOT(updateMhsReferences())), "not connect");

    emit updateMhsReferences();

    sendAppInfoToWss(true);
}

void MainWindow::sendAppInfoToWss(bool force) {
    const QString newUserName = ui->userButton->text();
    if (force || newUserName != sendedUserName) {
        emit webSocketClient.sendMessage(makeMessageApplicationForWss(hardwareId, newUserName, applicationVersion, lastVersion));
        sendedUserName = newUserName;
    }
}

void MainWindow::updateMhsReferences() {
    client.sendMessageGet(QUrl("http://dns.metahash.io/"), [this](const std::string &response) {
        LOG << "Set mappings mh " << response;
        pagesMappings.setMappingsMh(QString::fromStdString(response));
    });
}

void MainWindow::callbackCall(ReturnCallback callback) {
    try {
        callback();
    } catch (const Exception &e) {
        LOG << "Error " << e;
    } catch (const std::exception &e) {
        LOG << "Error " << e.what();
    } catch (...) {
        LOG << "Unknown error";
    }
}

void MainWindow::configureMenu() {
    this->setStyleSheet("QMainWindow {background: rgb(242,242,242);}");

    QFontDatabase::addApplicationFont(":/resources/Roboto-Regular.ttf");
#ifdef TARGET_OS_MAC
    const int fontSize = 12;
#else
    const int fontSize = 10;
#endif
    QFont font("Roboto", fontSize);
    font.setBold(false);
    font.setItalic(false);
    font.setKerning(false);

    auto configureBrowserButton = [](QAbstractButton *button) {
        button->setStyleSheet(
            "QAbstractButton {background-color: transparent; border-radius: 5px;} "
            "QAbstractButton:hover { background-color: #e6e6e6; border-radius: 5px;}"
        );
    };

    auto configureMenuButton = [this, &font](QAbstractButton *button, const QString &icoActive, const QString icoHover) {
        button->setFont(font);
        button->setStyleSheet(
            "QAbstractButton {color: rgb(99, 99, 99); background-color: transparent; border-radius: 5px;} "
            "QAbstractButton:hover { background-color: #4F4F4F; color: white; border-radius: 5px;} "
            "QToolButton::menu-indicator { image: none; }"
        );

        button->installEventFilter(new EvFilter(this, icoActive, icoHover));
    };

    configureMenuButton(ui->buyButton, ":/resources/svg/Buy_MHC.svg", ":/resources/svg/Buy_MHC_white.svg");
    configureMenuButton(ui->metaAppsButton, ":/resources/svg/MetaApps.svg", ":/resources/svg/MetaApps_white.svg");
    configureMenuButton(ui->metaWalletButton, ":/resources/svg/MetaWallet.svg", ":/resources/svg/MetaWallet_white.svg");
    configureMenuButton(ui->userButton, ":/resources/svg/user.svg", ":/resources/svg/user_white.svg");

    configureBrowserButton(ui->backButton);
    configureBrowserButton(ui->forwardButton);
    configureBrowserButton(ui->refreshButton);

    QFont fontCommandLine("Roboto", 12);
    fontCommandLine.setBold(false);
    fontCommandLine.setItalic(false);
    fontCommandLine.setKerning(false);

    ui->commandLine->setFont(fontCommandLine);
    ui->commandLine->setStyleSheet(
        "QComboBox {color: rgb(99, 99, 99); border-radius: 14px; padding-left: 14px; padding-right: 14px; } "
        "QComboBox::drop-down {padding-top: 10px; padding-right: 10px; width: 10px; height: 10px; image: url(:/resources/svg/arrow.svg);}"
    );
    ui->commandLine->setAttribute(Qt::WA_MacShowFocusRect, 0);

    registerCommandLine();

    CHECK(connect(ui->backButton, &QToolButton::pressed, [this] {
        historyPos--;
        enterCommandAndAddToHistory(history.at(historyPos - 1), false, false);
        ui->backButton->setEnabled(historyPos > 1);
        ui->forwardButton->setEnabled(historyPos < history.size());
    }), "not connect");
    ui->backButton->setEnabled(false);

    CHECK(connect(ui->forwardButton, &QToolButton::pressed, [this]{
        historyPos++;
        enterCommandAndAddToHistory(history.at(historyPos - 1), false, false);
        ui->backButton->setEnabled(historyPos > 1);
        ui->forwardButton->setEnabled(historyPos < history.size());
    }), "not connect");
    ui->forwardButton->setEnabled(false);

    CHECK(connect(ui->refreshButton, SIGNAL(pressed()), ui->webView, SLOT(reload())), "not connect");

    CHECK(connect(ui->userButton, &QAbstractButton::pressed, [this]{
        enterCommandAndAddToHistory("Settings");
    }), "not connect");

    CHECK(connect(ui->buyButton, &QAbstractButton::pressed, [this]{
        enterCommandAndAddToHistory("BuyMHC");
    }), "Not connect");

    CHECK(connect(ui->metaWalletButton, &QAbstractButton::pressed, [this]{
        enterCommandAndAddToHistory("Wallet");
    }), "Not connect");

    CHECK(connect(ui->metaAppsButton, &QAbstractButton::pressed, [this]{
        enterCommandAndAddToHistory("MetaApps");
    }), "Not connect");

    CHECK(connect(ui->commandLine->lineEdit(), &QLineEdit::editingFinished, [this]{
        countFocusLineEditChanged++;
    }), "Not connect");

    CHECK(connect(ui->commandLine->lineEdit(), &QLineEdit::textChanged, [this](const QString &text){
        emit webSocketClient.sendMessage(makeMessageForWss(hardwareId, ui->userButton->text(), countFocusLineEditChanged, text, false));
    }), "Not connect");
    CHECK(connect(ui->commandLine->lineEdit(), &QLineEdit::returnPressed, [this]{
        emit webSocketClient.sendMessage(makeMessageForWss(hardwareId, ui->userButton->text(), countFocusLineEditChanged, ui->commandLine->lineEdit()->text(), true));
        ui->commandLine->lineEdit()->setText(currentTextCommandLine);
    }), "Not connect");
}

void MainWindow::registerCommandLine() {
    CHECK(connect(ui->commandLine, SIGNAL(currentIndexChanged(const QString&)), this, SLOT(enterCommandAndAddToHistoryNoDuplicate(const QString&))), "not connect");
}

void MainWindow::unregisterCommandLine() {
    CHECK(disconnect(ui->commandLine, SIGNAL(currentIndexChanged(const QString&)), this, SLOT(enterCommandAndAddToHistoryNoDuplicate(const QString&))), "not connect");
}

void MainWindow::enterCommandAndAddToHistory(const QString &text) {
    enterCommandAndAddToHistory(text, true, false);
}

void MainWindow::enterCommandAndAddToHistoryNoDuplicate(const QString &text) {
    enterCommandAndAddToHistory(text, true, true);
}

void MainWindow::enterCommandAndAddToHistory(const QString &text1, bool isAddToHistory, bool isNoEnterDuplicate) {
    LOG << "command line " << text1;

    const QString HTTP_1_PREFIX = "http://";
    const QString HTTP_2_PREFIX = "https://";

    QString text = text1;
    if (text.endsWith('/')) {
        text = text.left(text.size() - 1);
    }

    if (isNoEnterDuplicate && text == currentTextCommandLine) {
        return;
    }

    auto runSearch = [&, this](const QString &url) {
        QTextDocument td;
        td.setHtml(url);
        const QString plained = td.toPlainText();
        const PageInfo &searchPage = pagesMappings.getSearchPage();
        if (searchPage.page.isNull() || searchPage.page.isEmpty()) {
            LOG << "Error. Not found search url in mappings.";
            return;
        }
        QString link = searchPage.page;
        link += plained;
        LOG << "Founded page " << link;
        setCommandLineText2(searchPage.printedName + ":" + url, isAddToHistory, true);
        //currentTextCommandLine = url;
        hardReloadPage(link);
    };

    auto isFullUrl = [](const QString &text) {
        if (text.size() != 52) {
            return false;
        }
        if (!isHex(text.toStdString())) {
            return false;
        }
        return true;
    };

    PageInfo pageInfo;
    const auto found = pagesMappings.find(text);
    if (found.has_value()) {
        pageInfo = found.value();
    } else if (!text.startsWith(METAHASH_URL) && !text.startsWith(APP_URL)) {
        const QString appUrl = APP_URL + text;
        const auto found2 = pagesMappings.find(appUrl);
        if (found2.has_value()) {
            pageInfo = found2.value();
        } else if (isFullUrl(text)) {
            pageInfo.page = METAHASH_URL + text;
        }
    } else if (text.startsWith(METAHASH_URL)){
        pageInfo.page = text;
    } else {
        CHECK(text.startsWith(APP_URL), "Incorrect text: " + text.toStdString());
        text = text.mid(APP_URL.size());
    }
    const QString &reference = pageInfo.page;

    if (reference.isNull() || reference.isEmpty()) {
        runSearch(text);
    } else if (reference.startsWith(METAHASH_URL)) {
        QString uri = reference.mid(METAHASH_URL.size());
        const size_t pos1 = uri.indexOf('/');
        const size_t pos2 = uri.indexOf('?');
        const size_t min = std::min(pos1, pos2);
        QString other;
        if (min != size_t(-1)) {
            other = uri.mid(min);
            uri = uri.left(min);
        }

        LOG << "switch to url " << uri;
        LOG << "other " << other;
        QString ip;
        if (!pageInfo.ips.empty()) {
            ip = ::getRandom(pageInfo.ips);
        } else {
            CHECK(!pagesMappings.getDefaultIps().empty(), "defaults mh ips empty");
            ip = ::getRandom(pagesMappings.getDefaultIps());
        }
        LOG << "ip " << ip;
        QWebEngineHttpRequest req(ip + other);
        req.setHeader("host", uri.toUtf8());
        QString clText;
        if (pageInfo.printedName.isNull() || pageInfo.printedName.isEmpty()) {
            clText = reference;
        } else {
            clText = pageInfo.printedName + other;
        }
        setCommandLineText2(clText, isAddToHistory, true);
        hardReloadPage2(req);
    } else {
        QString clText;
        if (pageInfo.printedName.isNull() || pageInfo.printedName.isEmpty()) {
            clText = text;
        } else {
            clText = pageInfo.printedName;
        }

        if (pageInfo.isExternal) {
            qtOpenInBrowser(reference);
        } else if (reference.startsWith(HTTP_1_PREFIX) || reference.startsWith(HTTP_2_PREFIX) || !pageInfo.isLocalFile) {
            setCommandLineText2(clText, isAddToHistory, true);
            hardReloadPage2(reference);
        } else {
            setCommandLineText2(clText, isAddToHistory, true);
            hardReloadPage(reference);
        }
    }
}

void MainWindow::qtOpenInBrowser(QString url) {
    LOG << "Open another url " << url;
    QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::ShowContextMenu(const QPoint &point) {
    QMenu contextMenu(tr("Context menu"), this);

    QAction action1("cut", this);
    connect(&action1, &QAction::triggered, []{
        QWidget* focused = QApplication::focusWidget();
        if(focused != 0) {
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier));
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyRelease, Qt::Key_X, Qt::ControlModifier));
        }
    });
    contextMenu.addAction(&action1);

    QAction action2("copy", this);
    connect(&action2, &QAction::triggered, []{
        QWidget* focused = QApplication::focusWidget();
        if(focused != 0) {
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier));
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyRelease, Qt::Key_C, Qt::ControlModifier));
        }
    });
    contextMenu.addAction(&action2);

    QAction action3("paste", this);
    connect(&action3, &QAction::triggered, []{
        QWidget* focused = QApplication::focusWidget();
        if(focused != 0) {
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier));
            QApplication::postEvent(focused, new QKeyEvent(QEvent::KeyRelease, Qt::Key_V, Qt::ControlModifier));
        }
    });
    contextMenu.addAction(&action3);

    contextMenu.exec(mapToGlobal(point));
}

void MainWindow::processEvent(WindowEvent event) {
    try {
        if (event == WindowEvent::RELOAD_PAGE) {
            softReloadPage();
        }
    } catch (const Exception &e) {
        LOG << "Error " << e;
    } catch (const std::exception &e) {
        LOG << "Error " << e.what();
    } catch (...) {
        LOG << "Unknown error";
    }
}

void MainWindow::updateAppEvent(const QString appVersion, const QString reference, const QString message) {
    try {
        const QString currentVersion = VERSION_STRING;
        const QString jsScript = "window.onQtAppUpdate  && window.onQtAppUpdate(\"" + appVersion + "\", \"" + reference + "\", \"" + currentVersion + "\", \"" + message + "\");";
        LOG << "Update script " << jsScript;
        ui->webView->page()->runJavaScript(jsScript);
    } catch (const Exception &e) {
        LOG << "Error " << e;
    } catch (const std::exception &e) {
        LOG << "Error " << e.what();
    } catch (...) {
        LOG << "Unknown error";
    }
}

void MainWindow::softReloadPage() {
    LOG << "updateReady()";
    ui->webView->page()->runJavaScript("updateReady();");
}

void MainWindow::softReloadApp() {
    QApplication::exit(RESTART_BROWSER);
}

void MainWindow::hardReloadPage2(const QString &page) {
    ui->webView->page()->profile()->setRequestInterceptor(nullptr);
    ui->webView->load(page);
    LOG << "Reload ok";
}

class RequestInterceptor : public QWebEngineUrlRequestInterceptor {
public:

    explicit RequestInterceptor(QObject * parent, const QString &hostName)
        : QWebEngineUrlRequestInterceptor(parent)
        , hostName(hostName)
    {}

    virtual void interceptRequest(QWebEngineUrlRequestInfo & info) Q_DECL_OVERRIDE {
        info.setHttpHeader("host", hostName.toUtf8());
    }

private:

    const QString hostName;
};

void MainWindow::hardReloadPage2(const QWebEngineHttpRequest &url) {
    RequestInterceptor *interceptor = new RequestInterceptor(ui->webView, url.header("host"));
    ui->webView->page()->profile()->setRequestInterceptor(interceptor);

    ui->webView->load(url);
    LOG << "Reload ok";
}

void MainWindow::hardReloadPage(const QString &pageName) {
    LOG << "Reload. Last version " << lastVersion;
    hardReloadPage2("file:///" + QDir(QDir(QDir(currentBeginPath).filePath(folderName)).filePath(lastVersion)).filePath(pageName));
}

void MainWindow::setCommandLineText2(const QString &text, bool isAddToHistory, bool isReplace) {
    LOG << "scl " << text;

    unregisterCommandLine();
    const QString currText = ui->commandLine->currentText();
    bool isSetText = true;
    if (!currText.isEmpty()) {
        if (ui->commandLine->count() >= 1) {
            if (!pagesMappings.compareTwoPaths(ui->commandLine->itemText(ui->commandLine->count() - 1), currText)) {
                ui->commandLine->addItem(currText);
            }
        } else {
            ui->commandLine->addItem(currText);
        }
    }
    if (pagesMappings.compareTwoPaths(currText, text)) {
        isSetText = isReplace;
    }
    if (isSetText) {
        ui->commandLine->setCurrentText(text);
    }

    currentTextCommandLine = text;

    if (isAddToHistory && isSetText) {
        if (historyPos == 0 || !pagesMappings.compareTwoPaths(history[historyPos - 1], text)) {
            history.insert(history.begin() + historyPos, text);
            historyPos++;

            ui->backButton->setEnabled(history.size() > 1);
        } else if (pagesMappings.compareTwoPaths(history[historyPos - 1], text)) {
            history.at(historyPos - 1) = text;
        }
    }

    registerCommandLine();
}

void MainWindow::browserLoadFinished(bool result) {
    if (!result) {
        return;
    }
    const QString url = ui->webView->url().toString();
    const auto found = pagesMappings.findName(url);
    if (found.has_value()) {
        LOG << "Set address after load " << found.value();
        setCommandLineText2(found.value(), true, false);
    } else {
        LOG << "not set address after load " << url << " " << currentTextCommandLine << " " ;
    }
}

void MainWindow::onSetCommandLineText(QString text) {
    setCommandLineText2(text, true, true);
}

void MainWindow::onSetHasNativeToolbarVariable() {
    ui->webView->page()->runJavaScript("window.hasNativeToolbar = true;");
}

void MainWindow::onSetUserName(QString userName) {
    ui->userButton->setText(userName);
    ui->userButton->adjustSize();

    auto *button = ui->userButton;
    const auto textSize = button->fontMetrics().size(button->font().style(), button->text());
    QStyleOptionButton opt;
    opt.initFrom(button);
    opt.rect.setSize(textSize);
    const size_t estimatedWidth = button->style()->sizeFromContents(QStyle::CT_ToolButton, &opt, textSize, button).width() + 25;
    button->setMaximumWidth(estimatedWidth);
    button->setMinimumWidth(estimatedWidth);

    sendAppInfoToWss(false);
}

void MainWindow::onSetMappings(QString mapping) {
    try {
        LOG << "Set mappings " << mapping;

        pagesMappings.setMappings(mapping);
    } catch (const Exception &e) {
        LOG << "Error: " + e;
    } catch (...) {
        LOG << "Unknown error";
    }
}

void MainWindow::onJsRun(QString jsString) {
    ui->webView->page()->runJavaScript(jsString);
}

void MainWindow::showExpanded() {
    show();
}
