#include <Geode/Geode.hpp>
#include <Geode/modify/ProfilePage.hpp>

#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/GJAccountManager.hpp>

#include <Geode/fmod/fmod.hpp>

#include <hiimjasmine00.user_data_api/include/UserDataAPI.hpp>

#include <unordered_map>

using namespace geode::prelude;

namespace {
    static std::unordered_map<int, int64_t> s_songCache;

    static constexpr int kMusicID = 0;

    static constexpr float kFadeOutSec = 0.25f;
    static constexpr float kFadeInSec  = 0.25f;

    static void uploadSongId(int64_t songId) {
        if (songId <= 0) return;
        auto data = matjson::Value::object();
        data["songId"] = songId;
        user_data::upload(std::move(data));
    }

    static bool isMyProfile(GJUserScore* score) {
        if (!score) return false;
        auto am = GJAccountManager::sharedState();
        int myAcc = am ? am->m_accountID : 0;
        return myAcc > 0 && score->m_accountID == myAcc;
    }

    static int64_t readSongIdFromScore(GJUserScore* score) {
        if (!score) return 0;

        auto const modID = Mod::get()->getID();
        if (!user_data::contains(score, modID)) return 0;

        auto res = user_data::get<matjson::Value>(score, modID);
        if (res.isErr()) return 0;

        auto const& v = res.unwrap();
        auto songRes = v["songId"].asInt();
        if (songRes.isErr()) return 0;

        auto songId = static_cast<int64_t>(songRes.unwrap());
        return songId > 0 ? songId : 0;
    }

    static CCLabelBMFont* ensureLabel(ProfilePage* page, char const* id, std::string const& text, CCPoint pos, float scale, int z) {
        auto label = typeinfo_cast<CCLabelBMFont*>(page->getChildByID(id));
        if (!label) {
            label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            label->setID(id);
            label->setScale(scale);
            label->setAnchorPoint({ 0.f, 0.5f });
            label->setPosition(pos);
            page->addChild(label, z);
        } else {
            label->setString(text.c_str());
        }
        return label;
    }

    static void setSongUI(ProfilePage* page, int64_t songId, bool downloading) {
        if (!page) return;

        std::string main = (songId > 0) ? fmt::format("Song: {}", songId) : std::string("Song: (none)");
        ensureLabel(page, "profile-song-label"_spr, main, {20.f, 32.f}, 0.35f, 3);

        if (songId > 0 && downloading) {
            ensureLabel(page, "profile-song-status"_spr, "Downloading...", {20.f, 18.f}, 0.28f, 3);
        } else {
            if (auto s = page->getChildByID("profile-song-status"_spr)) s->removeFromParent();
        }
    }

    static std::string menuLoopPath() {
        auto* fu = CCFileUtils::sharedFileUtils();
        auto mp3 = fu->fullPathForFilename("menuLoop.mp3", false);
        if (!mp3.empty()) return mp3;
        auto ogg = fu->fullPathForFilename("menuLoop.ogg", false);
        if (!ogg.empty()) return ogg;
        return {};
    }

    static void setChannelVolume(FMOD::Channel* ch, float v) {
        if (!ch) return;
        FMOD_Channel_SetVolume(reinterpret_cast<FMOD_CHANNEL*>(ch), v);
    }

    static float getChannelVolume(FMOD::Channel* ch) {
        if (!ch) return 1.f;
        float v = 1.f;
        FMOD_Channel_GetVolume(reinterpret_cast<FMOD_CHANNEL*>(ch), &v);
        return v;
    }
}

class $modify(UserThemeProfilePage, ProfilePage) {
    struct Fields {
        
        bool waitingUserData = false;
        float waitedUserData = 0.f;

        
        bool waitingSong = false;
        float waitedSong = 0.f;

        int64_t lastSongId = 0;
        std::string lastSongPath;

        
        bool fadeActive = false;
        float fadeT = 0.f;
        float fadeDur = 0.f;
        float fadeFrom = 1.f;
        float fadeTo = 1.f;

        
        bool menuVolSaved = false;
        float prevMenuVol = 1.f;

        
        enum class AfterFade {
            None,
            StartPreview,       
            RestoreMenuStart,   
            FadeInMenu,         
            FadeInPreview       
        } after = AfterFade::None;

        
        bool restoring = false;
    };

        void cancelAllTimers() {
        this->unschedule(schedule_selector(UserThemeProfilePage::pollUserDataReady));
        this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
        this->unschedule(schedule_selector(UserThemeProfilePage::tickFade));
        m_fields->waitingUserData = false;
        m_fields->waitingSong = false;
        m_fields->fadeActive = false;
        m_fields->after = Fields::AfterFade::None;
    }

    void restoreNow() {
        
        cancelAllTimers();

        auto eng = FMODAudioEngine::sharedEngine();
        eng->stopMusic(kMusicID);

        
        auto path = menuLoopPath();
        if (!path.empty()) {
            eng->playMusic(path, true, 0.f, kMusicID);
        }

        
        auto ch = eng->getActiveMusicChannel(kMusicID);
        if (ch) {
            float target = m_fields->menuVolSaved ? m_fields->prevMenuVol : 1.f;
            FMOD_Channel_SetVolume(reinterpret_cast<FMOD_CHANNEL*>(ch), target);
        }

        m_fields->restoring = false;
    }

    void startFade(float from, float to, float dur, Fields::AfterFade after) {
        m_fields->fadeActive = true;
        m_fields->fadeT = 0.f;
        m_fields->fadeDur = std::max(0.001f, dur);
        m_fields->fadeFrom = from;
        m_fields->fadeTo = to;
        m_fields->after = after;

        this->unschedule(schedule_selector(UserThemeProfilePage::tickFade));
        this->schedule(schedule_selector(UserThemeProfilePage::tickFade), 0.f);
    }

    void tickFade(float dt) {
        auto eng = FMODAudioEngine::sharedEngine();
        auto ch = eng->getActiveMusicChannel(kMusicID);
        if (!ch) {
            
            m_fields->fadeActive = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::tickFade));
            m_fields->after = Fields::AfterFade::None;
            return;
        }

        if (!m_fields->fadeActive) {
            this->unschedule(schedule_selector(UserThemeProfilePage::tickFade));
            return;
        }

        m_fields->fadeT += dt;
        float p = std::min(1.f, m_fields->fadeT / m_fields->fadeDur);
        float v = m_fields->fadeFrom + (m_fields->fadeTo - m_fields->fadeFrom) * p;
        setChannelVolume(ch, v);

        if (p < 1.f) return;

        
        m_fields->fadeActive = false;
        this->unschedule(schedule_selector(UserThemeProfilePage::tickFade));

        auto after = m_fields->after;
        m_fields->after = Fields::AfterFade::None;

        if (after == Fields::AfterFade::StartPreview) {
            
            eng->stopMusic(kMusicID);
            eng->playMusic(m_fields->lastSongPath, true, 0.f, kMusicID);

            
            auto ch2 = eng->getActiveMusicChannel(kMusicID);
            if (ch2) setChannelVolume(ch2, 0.f);
            startFade(0.f, 1.f, kFadeInSec, Fields::AfterFade::None);
            return;
        }

        if (after == Fields::AfterFade::RestoreMenuStart) {
            
            eng->stopMusic(kMusicID);

            auto path = menuLoopPath();
            if (!path.empty()) {
                eng->playMusic(path, true, 0.f, kMusicID);

                
                auto ch2 = eng->getActiveMusicChannel(kMusicID);
                if (ch2) setChannelVolume(ch2, 0.f);

                float target = m_fields->menuVolSaved ? m_fields->prevMenuVol : 1.f;
                startFade(0.f, target, kFadeInSec, Fields::AfterFade::None);
            }

            m_fields->restoring = false;
            return;
        }
    }

    void beginPreviewPlayback(std::string const& path) {
        if (path.empty()) return;

        auto eng = FMODAudioEngine::sharedEngine();
        auto ch = eng->getActiveMusicChannel(kMusicID);

        
        if (ch && !m_fields->menuVolSaved) {
            m_fields->prevMenuVol = getChannelVolume(ch);
            m_fields->menuVolSaved = true;
        }

        m_fields->lastSongPath = path;

        
        float from = ch ? getChannelVolume(ch) : 1.f;
        startFade(from, 0.f, kFadeOutSec, Fields::AfterFade::StartPreview);
    }

    void loadPageFromUserInfo(GJUserScore* score) {
        
        cancelAllTimers();
        
        restoreNow();
        
        ProfilePage::loadPageFromUserInfo(score);

        auto s = this->m_score ? this->m_score : score;
        if (!s) return;

        
        if (isMyProfile(s)) {
            auto mySong = Mod::get()->getSettingValue<int64_t>("profile-song-id");
            uploadSongId(mySong);
        }

        setSongUI(this, 0, false);

        
        m_fields->waitingUserData = true;
        m_fields->waitedUserData = 0.f;

        this->unschedule(schedule_selector(UserThemeProfilePage::pollUserDataReady));
        this->schedule(schedule_selector(UserThemeProfilePage::pollUserDataReady), 0.f);
    }

    void pollUserDataReady(float dt) {
        auto s = this->m_score;
        if (!m_fields->waitingUserData || !s) {
            m_fields->waitingUserData = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollUserDataReady));
            return;
        }

        m_fields->waitedUserData += dt;
        if (m_fields->waitedUserData >= 5.f) {
            m_fields->waitingUserData = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollUserDataReady));
            return;
        }

        if (!user_data::contains(s, Mod::get()->getID())) return;

        m_fields->waitingUserData = false;
        this->unschedule(schedule_selector(UserThemeProfilePage::pollUserDataReady));

        int64_t songId = readSongIdFromScore(s);
        if (s->m_accountID > 0) s_songCache[s->m_accountID] = songId;

        m_fields->lastSongId = songId;

        if (songId <= 0) {
            setSongUI(this, 0, false);
            return;
        }

        auto mdm = MusicDownloadManager::sharedState();
        if (!mdm) {
            setSongUI(this, songId, false);
            return;
        }

        bool downloaded = mdm->isSongDownloaded(static_cast<int>(songId));
        if (!downloaded) {
            mdm->downloadSong(static_cast<int>(songId));
        }

        setSongUI(this, songId, !downloaded);

        
        m_fields->waitingSong = true;
        m_fields->waitedSong = 0.f;

        this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
        this->schedule(schedule_selector(UserThemeProfilePage::pollSongReady), 0.1f);
    }

    void pollSongReady(float dt) {
        auto s = this->m_score;
        if (!m_fields->waitingSong || !s) {
            m_fields->waitingSong = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
            return;
        }

        m_fields->waitedSong += dt;
        if (m_fields->waitedSong >= 10.f) {
            setSongUI(this, m_fields->lastSongId, false);
            m_fields->waitingSong = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
            return;
        }

        auto songId = m_fields->lastSongId;
        if (songId <= 0) {
            setSongUI(this, 0, false);
            m_fields->waitingSong = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
            return;
        }

        auto mdm = MusicDownloadManager::sharedState();
        if (!mdm) return;

        if (!mdm->isSongDownloaded(static_cast<int>(songId))) {
            setSongUI(this, songId, true);
            return;
        }

        
        setSongUI(this, songId, false);

        auto path = mdm->pathForSong(static_cast<int>(songId));
        if (!path.empty()) {
            m_fields->waitingSong = false;
            this->unschedule(schedule_selector(UserThemeProfilePage::pollSongReady));
            beginPreviewPlayback(path);
        }
    }

    void onClose(CCObject* sender) {
        restoreNow();
        ProfilePage::onClose(sender);
    }

    void keyBackClicked() {
        restoreNow();
        ProfilePage::keyBackClicked();
    }

    void onExit() {
        restoreNow();
        ProfilePage::onExit();
    }
};