/***************************************************************************
 *            screenscraper.cpp
 *
 *  Wed Jun 18 12:00:00 CEST 2017
 *  Copyright 2017 Lars Muldjord
 *  muldjordlars@gmail.com
 ****************************************************************************/
/*
 *  This file is part of skyscraper.
 *
 *  skyscraper is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  skyscraper is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with skyscraper; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include "screenscraper.h"

#include "crc32.h"
#include "platform.h"
#include "strtools.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>

constexpr int RETRIESMAX = 4;
constexpr int MINARTSIZE = 256;

ScreenScraper::ScreenScraper(Settings *config,
                             QSharedPointer<NetManager> manager)
    : AbstractScraper(config, manager) {
    connect(&limitTimer, &QTimer::timeout, &limiter, &QEventLoop::quit);
    limitTimer.setInterval(
        1200); // 1.2 second request limit set a bit above 1.0 as requested by
               // the good folks at ScreenScraper. Don't change!
    limitTimer.setSingleShot(false);
    limitTimer.start();

    baseUrl = "http://www.screenscraper.fr";

    fetchOrder.append(PUBLISHER);
    fetchOrder.append(DEVELOPER);
    fetchOrder.append(PLAYERS);
    fetchOrder.append(AGES);
    fetchOrder.append(RATING);
    fetchOrder.append(DESCRIPTION);
    fetchOrder.append(RELEASEDATE);
    fetchOrder.append(TAGS);
    if (config->includeMedia != 2) {
        fetchOrder.append(SCREENSHOT);
        fetchOrder.append(COVER);
        fetchOrder.append(WHEEL);
        fetchOrder.append(MARQUEE);
        fetchOrder.append(TEXTURE);
        fetchOrder.append(VIDEO);
    }
}

void ScreenScraper::getSearchResults(QList<GameEntry> &gameEntries,
                                     QString searchName, QString) {
    int platformId = getPlatformId(config->platform);
    if (platformId == -1) {
        reqRemaining = 0;
        printf("\033[0;31mPlatform not supported by ScreenScraper or it hasn't "
               "yet been included in Skyscraper for this module...\033[0m\n");
        return;
    }

    QString gameUrl =
        "https://www.screenscraper.fr/api2/"
        "jeuInfos.php?devid=muldjord&devpassword=" +
        StrTools::unMagic(
            "204;198;236;130;203;181;203;126;191;167;200;198;192;228;169;156") +
        "&softname=skyscraper" VERSION +
        (config->user.isEmpty() ? "" : "&ssid=" + config->user) +
        (config->password.isEmpty() ? "" : "&sspassword=" + config->password) +
        (platformId == -1 ? "" : "&systemeid=" + QString::number(platformId)) +
        "&output=json&" + searchName;

    for (int retries = 0; retries < RETRIESMAX; ++retries) {
        limiter.exec();
        netComm->request(gameUrl);
        q.exec();
        data = netComm->getData();

        QByteArray headerData =
            data.left(1024); // Minor optimization with minimal more RAM usage
        // Do error checks on headerData. It's more stable than checking the
        // potentially faulty JSON
        if (headerData.isEmpty()) {
            printf("\033[1;33mRetrying request...\033[0m\n\n");
            continue;
        } else if (headerData.contains("non trouvée")) {
            return;
        } else if (headerData.contains("API totalement fermé")) {
            printf("\033[1;31mThe ScreenScraper API is currently closed, "
                   "exiting nicely...\033[0m\n\n");
            reqRemaining = 0;
            return;
        } else if (headerData.contains(
                       "Le logiciel de scrape utilisé a été blacklisté")) {
            printf("\033[1;31mSkyscraper has apparently been blacklisted at "
                   "ScreenScraper, exiting nicely...\033[0m\n\n");
            reqRemaining = 0;
            return;
        } else if (headerData.contains("Votre quota de scrape est")) {
            printf("\033[1;31mYour daily ScreenScraper request limit has been "
                   "reached, exiting nicely...\033[0m\n\n");
            reqRemaining = 0;
            return;
        } else if (
            headerData.contains("API fermé pour les non membres") ||
            headerData.contains("API closed for non-registered members") ||
            headerData.contains(
                "****T****h****e**** ****m****a****x****i****m****u****m**** "
                "****t****h****r****e****a****d****s**** "
                "****a****l****l****o****w****e****d**** ****t****o**** "
                "****l****e****e****c****h****e****r**** "
                "****u****s****e****r****s**** ****i****s**** "
                "****a****l****r****e****a****d****y**** "
                "****u****s****e****d****")) {
            printf(
                "\033[1;31mThe screenscraper service is currently closed or "
                "too busy to handle requests from unregistered and inactive "
                "users. Sign up for an account at https://www.screenscraper.fr "
                "and contribute to gain more threads. Then use the credentials "
                "with Skyscraper using the '-u user:pass' command line option "
                "or by setting 'userCreds=\"user:pass\"' in "
                "'/home/<USER>/.skyscraper/config.ini'.\033[0m\n\n");
            if (retries == RETRIESMAX - 1) {
                reqRemaining = 0;
                return;
            } else {
                continue;
            }
        }

        // Fix faulty JSON that is sometimes received back from ScreenScraper
        data.replace("],\n\t\t}", "]\n\t\t}");

        // Now parse the JSON
        jsonObj = QJsonDocument::fromJson(data).object();

        // Check if we got a valid JSON document back
        if (jsonObj.isEmpty()) {
            printf("\033[1;31mScreenScraper APIv2 returned invalid / empty "
                   "Json. Their servers are probably down. Please try again "
                   "later or use a different scraping module with '-s MODULE'. "
                   "Check 'Skyscraper --help' for more information.\033[0m\n");
            data.replace(StrTools::unMagic("204;198;236;130;203;181;203;126;"
                                           "191;167;200;198;192;228;169;156"),
                         "****");
            data.replace(config->password.toUtf8(), "****");
            QFile jsonErrorFile("./screenscraper_error.json");
            if (jsonErrorFile.open(QIODevice::WriteOnly)) {
                if (data.length() > 64) {
                    jsonErrorFile.write(data);
                    printf("The erroneous answer was written to "
                           "'/home/<USER>/.skyscraper/screenscraper_error.json'. "
                           "If this file contains game data, please consider "
                           "filing a bug report at "
                           "'https://github.com/Gemba/skyscraper/issues' and "
                           "attach that file.\n");
                }
                jsonErrorFile.close();
            }
            break; // DON'T try again! If we don't get a valid JSON document,
                   // something is very wrong with the API
        }

        // Check if the request was successful
        if (jsonObj["header"].toObject()["success"].toString() != "true") {
            printf("Request returned a success state of '%s'. Error was:\n%s\n",
                   jsonObj["header"]
                       .toObject()["success"]
                       .toString()
                       .toStdString()
                       .c_str(),
                   jsonObj["header"]
                       .toObject()["error"]
                       .toString()
                       .toStdString()
                       .c_str());
            // Try again. We handle important errors above, so something weird
            // is going on here
            continue;
        }

        // Check if user has exceeded daily request limit
        if (!jsonObj["response"]
                 .toObject()["ssuser"]
                 .toObject()["requeststoday"]
                 .toString()
                 .isEmpty() &&
            !jsonObj["response"]
                 .toObject()["ssuser"]
                 .toObject()["maxrequestsperday"]
                 .toString()
                 .isEmpty()) {
            reqRemaining = jsonObj["response"]
                               .toObject()["ssuser"]
                               .toObject()["maxrequestsperday"]
                               .toString()
                               .toInt() -
                           jsonObj["response"]
                               .toObject()["ssuser"]
                               .toObject()["requeststoday"]
                               .toString()
                               .toInt();
            if (reqRemaining <= 0) {
                printf("\033[1;31mYour daily ScreenScraper request limit has "
                       "been reached, exiting nicely...\033[0m\n\n");
            }
        }

        // Check if we got a game entry back
        if (jsonObj["response"].toObject().contains("jeu")) {
            // Game found, stop retrying
            break;
        }
    }

    jsonObj = jsonObj["response"].toObject()["jeu"].toObject();

    GameEntry game;
    game.title = getJsonText(jsonObj["noms"].toArray(), REGION);

    // 'screenscraper' sometimes returns a faulty result with the following
    // names. If we get either result DON'T use it. It will provide faulty data
    // for the cache
    if ((game.title.toLower().contains("hack") &&
         game.title.toLower().contains("link")) ||
        game.title.toLower() == "zzz" ||
        game.title.toLower().contains("notgame")) {
        return;
    }

    // If title still unset, no acceptable rom was found, so return with no
    // results
    if (game.title.isNull()) {
        return;
    }

    game.url = gameUrl;
    game.platform = jsonObj["systeme"].toObject()["text"].toString();

    // Only check if platform is empty, it's always correct when using
    // ScreenScraper
    if (!game.platform.isEmpty())
        gameEntries.append(game);
}

void ScreenScraper::getGameData(GameEntry &game) {
    populateGameEntry(game);
}

void ScreenScraper::getReleaseDate(GameEntry &game) {
    game.releaseDate = getJsonText(jsonObj["dates"].toArray(), REGION);
}

void ScreenScraper::getDeveloper(GameEntry &game) {
    game.developer = jsonObj["developpeur"].toObject()["text"].toString();
}

void ScreenScraper::getPublisher(GameEntry &game) {
    game.publisher = jsonObj["editeur"].toObject()["text"].toString();
}

void ScreenScraper::getDescription(GameEntry &game) {
    game.description = getJsonText(jsonObj["synopsis"].toArray(), LANGUE);
}

void ScreenScraper::getPlayers(GameEntry &game) {
    game.players = jsonObj["joueurs"].toObject()["text"].toString();
}

void ScreenScraper::getAges(GameEntry &game) {
    QList<QString> ageBoards;
    ageBoards.append("PEGI");
    ageBoards.append("ESRB");
    ageBoards.append("SS");

    if (!jsonObj["classifications"].isArray())
        return;

    QJsonArray jsonAges = jsonObj["classifications"].toArray();

    for (const auto &ageBoard : ageBoards) {
        for (int a = 0; a < jsonAges.size(); ++a) {
            if (jsonAges.at(a).toObject()["type"].toString() == ageBoard) {
                game.ages = jsonAges.at(a).toObject()["text"].toString();
                return;
            }
        }
    }
}

void ScreenScraper::getRating(GameEntry &game) {
    game.rating = jsonObj["note"].toObject()["text"].toString();
    bool toDoubleOk = false;
    double rating = game.rating.toDouble(&toDoubleOk);
    if (toDoubleOk) {
        game.rating = QString::number(rating / 20.0);
    } else {
        game.rating = "";
    }
}

void ScreenScraper::getTags(GameEntry &game) {
    if (!jsonObj["genres"].isArray())
        return;

    QJsonArray jsonTags = jsonObj["genres"].toArray();

    for (int a = 0; a < jsonTags.size(); ++a) {
        QString tag =
            getJsonText(jsonTags.at(a).toObject()["noms"].toArray(), LANGUE);
        if (!tag.isEmpty()) {
            game.tags.append(tag + ", ");
        }
    }

    game.tags.chop(2);
}

void ScreenScraper::getCover(GameEntry &game) {
    QString url = "";
    if (config->platform == "arcade" || config->platform == "fba" ||
        config->platform == "mame-advmame" ||
        config->platform == "mame-libretro" ||
        config->platform == "mame-mame4all") {
        url = getJsonText(jsonObj["medias"].toArray(), REGION,
                          QList<QString>({"flyer"}));
    } else {
        url = getJsonText(jsonObj["medias"].toArray(), REGION,
                          QList<QString>({"box-2D"}));
    }
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            QImage image;
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                netComm->getData().size() >= MINARTSIZE &&
                image.loadFromData(netComm->getData())) {
                game.coverData = netComm->getData();
            } else {
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

void ScreenScraper::getScreenshot(GameEntry &game) {
    QString url = getJsonText(jsonObj["medias"].toArray(), REGION,
                              QList<QString>({"ss", "sstitle"}));
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            QImage image;
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                netComm->getData().size() >= MINARTSIZE &&
                image.loadFromData(netComm->getData())) {
                game.screenshotData = netComm->getData();
            } else {
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

void ScreenScraper::getWheel(GameEntry &game) {
    QString url = getJsonText(jsonObj["medias"].toArray(), REGION,
                              QList<QString>({"wheel", "wheel-hd"}));
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            QImage image;
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                netComm->getData().size() >= MINARTSIZE &&
                image.loadFromData(netComm->getData())) {
                game.wheelData = netComm->getData();
            } else {
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

void ScreenScraper::getMarquee(GameEntry &game) {
    QString url = getJsonText(jsonObj["medias"].toArray(), REGION,
                              QList<QString>({"screenmarquee"}));
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            QImage image;
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                netComm->getData().size() >= MINARTSIZE &&
                image.loadFromData(netComm->getData())) {
                game.marqueeData = netComm->getData();
            } else {
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

void ScreenScraper::getTexture(GameEntry &game) {
    QString url = getJsonText(
        jsonObj["medias"].toArray(), REGION,
        QList<QString>({"support-2D", "support-2d", "support-texture"}));
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            QImage image;
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                netComm->getData().size() >= MINARTSIZE &&
                image.loadFromData(netComm->getData())) {
                game.textureData = netComm->getData();
            } else {
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

void ScreenScraper::getVideo(GameEntry &game) {
    QStringList types;
    if (config->videoPreferNormalized) {
        types.append("video-normalized");
    }
    types.append("video");
    QString url = getJsonText(jsonObj["medias"].toArray(), NONE, types);
    if (!url.isEmpty()) {
        bool moveOn = true;
        for (int retries = 0; retries < RETRIESMAX; ++retries) {
            limiter.exec();
            netComm->request(url);
            q.exec();
            game.videoData = netComm->getData();
            // Make sure received data is actually a video file
            QByteArray contentType = netComm->getContentType();
            if (netComm->getError(config->verbosity) ==
                    QNetworkReply::NoError &&
                contentType.contains("video/") &&
                game.videoData.size() > 4096) {
                game.videoFormat = contentType.mid(
                    contentType.indexOf("/") + 1,
                    contentType.length() - contentType.indexOf("/") + 1);
            } else {
                game.videoData = "";
                moveOn = false;
            }
            if (moveOn)
                break;
        }
    }
}

QList<QString> ScreenScraper::getSearchNames(const QFileInfo &info) {
    QList<QString> hashList;
    QCryptographicHash md5(QCryptographicHash::Md5);
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    Crc32 crc;
    crc.initInstance(1);

    bool unpack = config->unpack;

    if (unpack) {
        // Size limit for "unpack" is set to 8 megs to ensure the pi doesn't run
        // out of memory
        if ((info.suffix() == "7z" || info.suffix() == "zip") &&
            info.size() < 81920000) {
            // For 7z (7z, zip) unpacked file reading
            {
                QProcess decProc;
                decProc.setReadChannel(QProcess::StandardOutput);
                decProc.start(
                    "7z", QStringList({"l", "-so", info.absoluteFilePath()}));
                if (decProc.waitForFinished(30000)) {
                    if (decProc.exitStatus() != QProcess::NormalExit) {
                        printf("Getting file list from compressed file failed, "
                               "falling back...\n");
                        unpack = false;
                    } else if (!decProc.readAllStandardOutput().contains(
                                   " 1 files")) {
                        printf("Compressed file contains more than 1 file, "
                               "falling back...\n");
                        unpack = false;
                    }
                } else {
                    printf("Getting file list from compressed file timed out "
                           "or failed, falling back...\n");
                    unpack = false;
                }
            }

            if (unpack) {
                QProcess decProc;
                decProc.setReadChannel(QProcess::StandardOutput);

                decProc.start(
                    "7z", QStringList({"x", "-so", info.absoluteFilePath()}));
                if (decProc.waitForFinished(30000)) {
                    if (decProc.exitStatus() == QProcess::NormalExit) {
                        QByteArray allData = decProc.readAllStandardOutput();
                        md5.addData(allData);
                        sha1.addData(allData);
                        crc.pushData(1, allData.data(), allData.length());
                    } else {
                        printf("Something went wrong when decompressing file "
                               "to stdout, falling back...\n");
                        unpack = false;
                    }
                } else {
                    printf("Decompression process timed out or failed, falling "
                           "back...\n");
                    unpack = false;
                }
            }
        } else {
            printf("File either not a compressed file or exceeds 8 meg size "
                   "limit, falling back...\n");
            unpack = false;
        }
    }

    if (!unpack) {
        // For normal file reading
        QFile romFile(info.absoluteFilePath());
        romFile.open(QIODevice::ReadOnly);
        while (!romFile.atEnd()) {

            QByteArray dataSeg = romFile.read(1024);
            md5.addData(dataSeg);
            sha1.addData(dataSeg);
            crc.pushData(1, dataSeg.data(), dataSeg.length());
        }
        romFile.close();
    }

    QString crcResult = QString::number(crc.releaseInstance(1), 16);
    while (crcResult.length() < 8) {
        crcResult.prepend("0");
    }
    QString md5Result = md5.result().toHex();
    while (md5Result.length() < 32) {
        md5Result.prepend("0");
    }
    QString sha1Result = sha1.result().toHex();
    while (sha1Result.length() < 40) {
        sha1Result.prepend("0");
    }

    // For some reason the APIv2 example from their website does not url encode
    // '(' and ')' so I've excluded them
    hashList.append(QUrl::toPercentEncoding(info.fileName(), "()"));
    hashList.append(crcResult.toUpper());
    hashList.append(md5Result.toUpper());
    hashList.append(sha1Result.toUpper());

    QList<QString> searchNames;
    if (info.size() != 0) {
        searchNames.append("crc=" + hashList.at(1) + "&md5=" + hashList.at(2) +
                           "&sha1=" + hashList.at(3) +
                           "&romnom=" + hashList.at(0) +
                           "&romtaille=" + QString::number(info.size()));
    } else {
        searchNames.append("romnom=" + hashList.at(0));
    }

    return searchNames;
}

QString ScreenScraper::getJsonText(QJsonArray jsonArr, int attr,
                                   QList<QString> types) {
    if (attr == NONE && !types.isEmpty()) {
        for (const auto &type : types) {
            for (const auto &jsonVal : jsonArr) {
                if (jsonVal.toObject()["type"].toString() == type) {
                    if (jsonVal.toObject()["url"].isString()) {
                        return jsonVal.toObject()["url"].toString();
                    } else {
                        return jsonVal.toObject()["text"].toString();
                    }
                }
            }
        }
    } else if (attr == REGION) {
        // Not using the config->regionPrios since they might have changed due
        // to region autodetection. So using temporary internal one instead.
        for (const auto &region : regionPrios) {
            if (types.isEmpty()) {
                for (const auto &jsonVal : jsonArr) {
                    if (jsonVal.toObject()["region"].toString() == region) {
                        if (jsonVal.toObject()["url"].isString()) {
                            return jsonVal.toObject()["url"].toString();
                        } else {
                            return jsonVal.toObject()["text"].toString();
                        }
                    }
                }
            } else {
                for (const auto &type : types) {
                    for (const auto &jsonVal : jsonArr) {
                        if (jsonVal.toObject()["region"].toString() == region &&
                            jsonVal.toObject()["type"].toString() == type) {
                            if (jsonVal.toObject()["url"].isString()) {
                                return jsonVal.toObject()["url"].toString();
                            } else {
                                return jsonVal.toObject()["text"].toString();
                            }
                        }
                    }
                }
            }
        }
    } else if (attr == LANGUE) {
        for (const auto &lang : config->langPrios) {
            if (types.isEmpty()) {
                for (const auto &jsonVal : jsonArr) {
                    if (jsonVal.toObject()["langue"].toString() == lang) {
                        if (jsonVal.toObject()["url"].isString()) {
                            return jsonVal.toObject()["url"].toString();
                        } else {
                            return jsonVal.toObject()["text"].toString();
                        }
                    }
                }
            } else {
                for (const auto &type : types) {
                    for (const auto &jsonVal : jsonArr) {
                        if (jsonVal.toObject()["langue"].toString() == lang &&
                            jsonVal.toObject()["type"].toString() == type) {
                            if (jsonVal.toObject()["url"].isString()) {
                                return jsonVal.toObject()["url"].toString();
                            } else {
                                return jsonVal.toObject()["text"].toString();
                            }
                        }
                    }
                }
            }
        }
    }
    return QString();
}

int ScreenScraper::getPlatformId(const QString platform) {
    return Platform::get().getPlatformIdOnScraper(platform, config->scraper);
}