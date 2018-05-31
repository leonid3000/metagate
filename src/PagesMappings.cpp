#include "PagesMappings.h"

#include "Log.h"
#include "check.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonObject>

const QString METAHASH_URL = "mh://";
const QString APP_URL = "app://";

PagesMappings::PagesMappings(){

}

PagesMappings::Name::Name(const QString &text) {
    QString txt = text;
    if (txt.endsWith('/')) {
        txt = txt.left(txt.size() - 1);
    }
    name = txt.toLower();
}

bool PagesMappings::Name::operator<(const Name &second) const {
    return this->name < second.name;
}

bool PagesMappings::Name::operator==(const Name &second) const {
    return this->name == second.name;
}

static QString ipToHttp(const QString &ip) {
    const static QString HTTP = "http://";
    if (ip.startsWith(HTTP)) {
        return ip;
    } else {
        return HTTP + ip;
    }
}

void PagesMappings::setMappingsMh(QString mapping) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(mapping.toUtf8(), &parseError);
    CHECK(parseError.error == QJsonParseError::NoError, "Json parse error: " + parseError.errorString().toStdString());
    const QJsonObject root = document.object();
    CHECK(root.contains("ext") && root.value("ext").isArray(), "ext field not found ");
    const QJsonArray &routes = root.value("ext").toArray();
    for (const QJsonValue &value: routes) {
        CHECK(value.isObject(), "value array incorrect type");
        const QJsonObject &element = value.toObject();
        CHECK(element.contains("type") && element.value("type").isString(), "type field not found");
        const QString type = element.value("type").toString();
        const bool isDefault = type == "defaultGateway";
        if (!isDefault) {
            CHECK(element.contains("url") && element.value("url").isString(), "url field not found");
            const QString url = element.value("url").toString();
            CHECK(element.contains("name") && element.value("name").isString(), "name field not found");
            const QString name = element.value("name").toString();
            CHECK(element.contains("isExternal") && element.value("isExternal").isBool(), "isExternal field not found");
            const bool isExternal = element.value("isExternal").toBool();
            std::vector<QString> ips;
            if (element.contains("ip") && element.value("ip").isArray()) {
                for (const QJsonValue &ip: element.value("ip").toArray()) {
                    CHECK(ip.isString(), "ips array incorrect type");
                    ips.emplace_back(ipToHttp(ip.toString()));
                }
            }

            PageInfo page(url, isExternal, isDefault, false);
            page.printedName = METAHASH_URL + name;
            page.ips = ips;

            auto addToMap = [this](auto &map, const QString &key, const PageInfo &page) {
                const Name name(key);
                auto found = map.find(name);
                if (found == map.end() || found->second.page.startsWith(METAHASH_URL)) { // Данные из javascript имеют приоритет
                    map[name] = page;
                }
            };

            addToMap(mappingsPages, name, page); // TODO заменить на shared_ptr или придумать другую схему
            addToMap(mappingsPages, url, page);
            addToMap(mappingsPages, page.printedName, page);

            if (element.contains("aliases") && element.value("aliases").isArray()) {
                for (const QJsonValue &alias: element.value("aliases").toArray()) {
                    CHECK(alias.isString(), "aliases array incorrect type");
                    addToMap(mappingsPages, alias.toString(), page);
                }
            }
        } else {
            if (element.contains("ip") && element.value("ip").isArray()) {
                std::vector<QString> ips;
                for (const QJsonValue &ip: element.value("ip").toArray()) {
                    CHECK(ip.isString(), "ips array incorrect type");
                    ips.emplace_back(ipToHttp(ip.toString()));
                }
                defaultMhIps = ips;
            }
        }
    }
}

void PagesMappings::setMappings(QString mapping) {
    const QJsonDocument document = QJsonDocument::fromJson(mapping.toUtf8());
    const QJsonObject root = document.object();
    CHECK(root.contains("routes") && root.value("routes").isArray(), "routes field not found");
    const QJsonArray &routes = root.value("routes").toArray();
    for (const QJsonValue &value: routes) {
        CHECK(value.isObject(), "value array incorrect type");
        const QJsonObject &element = value.toObject();
        CHECK(element.contains("url") && element.value("url").isString(), "url field not found");
        const QString url = element.value("url").toString();
        CHECK(element.contains("name") && element.value("name").isString(), "name field not found");
        const QString name = element.value("name").toString();
        CHECK(element.contains("isExternal") && element.value("isExternal").isBool(), "isExternal field not found");
        const bool isExternal = element.value("isExternal").toBool();
        bool isDefault = false;
        if (element.contains("isDefault") && element.value("isDefault").isBool()) {
            isDefault = element.value("isDefault").toBool();
        }
        bool isPreferred = false;
        if (element.contains("isPreferred") && element.value("isPreferred").isBool()) {
            isPreferred = element.value("isPreferred").toBool();
        }
        bool isLocalFile = true;
        if (element.contains("isLocalFile") && element.value("isLocalFile").isBool()) {
            isLocalFile = element.value("isLocalFile").toBool();
        }

        PageInfo pageInfo(url, isExternal, isDefault, isLocalFile);
        if (!name.startsWith(APP_URL) && !name.startsWith(METAHASH_URL)) {
            pageInfo.printedName = APP_URL + name;
        } else {
            pageInfo.printedName = name;
        }
        mappingsPages[Name(name)] = pageInfo;

        auto foundUrlToName = urlToName.find(url);
        if (foundUrlToName == urlToName.end()) {
            urlToName[url] = pageInfo.printedName;
        } else if (isPreferred) {
            urlToName[url] = pageInfo.printedName;
        }
    }
}

struct PathParsed {
    enum class Type {
        METAHASH, APP, NONE
    };

    QString path;

    Type type;

    PathParsed(const QString &url) {
        if (url.startsWith(METAHASH_URL)) {
            type = Type::METAHASH;
            path = url.mid(METAHASH_URL.size());
        } else if (url.startsWith(APP_URL)) {
            type = Type::APP;
            path = url.mid(APP_URL.size());
        } else {
            type = Type::NONE;
            path = url;
        }
    }
};

bool PagesMappings::compareTwoPaths(const QString &path1, const QString &path2) const {
    PathParsed p1(path1);
    PathParsed p2(path2);

    if (p1.type == p2.type || p1.type == PathParsed::Type::NONE || p2.type == PathParsed::Type::NONE) {
        const Name lowerPath1(p1.path.toLower());
        const Name lowerPath2(p2.path.toLower());
        const auto found1 = mappingsPages.find(lowerPath1);
        const auto found2 = mappingsPages.find(lowerPath2);
        if (found1 == mappingsPages.end() || found2 == mappingsPages.end()) {
            return lowerPath1 == lowerPath2;
        } else {
            return found1->second.page == found2->second.page;
        }
    } else {
        return false;
    }
}

const PageInfo& PagesMappings::getSearchPage() const {
    for (const auto &pageInfoIt: mappingsPages) {
        if (pageInfoIt.second.isDefault) {
            return pageInfoIt.second;
        }
    }
    throwErr("Not found search page");
}

Optional<PageInfo> PagesMappings::find(const QString &url) const {
    const auto found = mappingsPages.find(Name(url));
    if (found == mappingsPages.end()) {
        return Optional<PageInfo>();
    } else {
        return Optional<PageInfo>(found->second);
    }
}

const std::vector<QString>& PagesMappings::getDefaultIps() const {
    return defaultMhIps;
}

Optional<QString> PagesMappings::findName(const QString &url) const {
    auto found = urlToName.find(url);
    if (found == urlToName.end()) {
        const int find = url.lastIndexOf('/');
        if (find != -1) {
            const QString lastUrl = url.mid(find + 1);
            found = urlToName.find(lastUrl);
        }
    }
    if (found == urlToName.end()) {
        return Optional<QString>();
    } else {
        return Optional<QString>(found->second);
    }
}