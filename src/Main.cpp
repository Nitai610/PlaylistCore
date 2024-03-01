#include "Main.hpp"
#include "Types/SongDownloaderAddon.hpp"
#include "Types/Scroller.hpp"
#include "Types/Config.hpp"
#include "PlaylistCore.hpp"
#include "Settings.hpp"
#include "Utils.hpp"
#include "ResettableStaticPtr.hpp"

#include <chrono>

#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "songloader/shared/API.hpp"

#include "bsml/shared/BSML.hpp"

#include "custom-types/shared/delegate.hpp"

#include "GlobalNamespace/StandardLevelDetailViewController.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LevelPackDetailViewController.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/BeatmapDifficultySegmentedControlController.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsViewController.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsGridView.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsGridViewAnimator.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionCell.hpp"
#include "GlobalNamespace/PageControl.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/ISpriteAsyncLoader.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/PreviewDifficultyBeatmapSet.hpp"
#include "GlobalNamespace/IDifficultyBeatmap.hpp"
#include "GlobalNamespace/IBeatmapLevel.hpp"

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Rect.hpp" // This needs to be included before RectTransform
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Events/UnityAction.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/InputFieldView.hpp"
#include "Tweening/TimeTweeningManager.hpp"
#include "Tweening/Vector2Tween.hpp"
#include "Zenject/DiContainer.hpp"
#include "Zenject/StaticMemoryPool_7.hpp"
#include "System/Tuple_2.hpp"
#include "System/Action_1.hpp"
#include "System/Action_2.hpp"
#include "System/Collections/Generic/HashSet_1.hpp"

using namespace GlobalNamespace;
using namespace PlaylistCore;

modloader::ModInfo modInfo = {"PlaylistCore", VERSION, 0};
modloader::ModInfo managerModInfo = {"PlaylistManager", VERSION, 0};

bool hasManager;

Logger& getLogger() {
    static auto logger = new Logger(modInfo, LoggerOptions(false, true));
    return *logger;
}

std::string GetPlaylistsPath() {
    static std::string playlistsPath(getDataDir(managerModInfo) + "Playlists");
    return playlistsPath;
}

std::string GetBackupsPath() {
    static std::string backupsPath(getDataDir(managerModInfo) + "PlaylistBackups");
    return backupsPath;
}

std::string GetCoversPath() {
    static std::string coversPath(getDataDir(managerModInfo) + "Covers");
    return coversPath;
}

// small fix for horizontal tables
MAKE_HOOK_MATCH(TableView_ReloadDataKeepingPosition, &HMUI::TableView::ReloadDataKeepingPosition,
        void, HMUI::TableView* self) {

    self->ReloadData();

    auto rect = self->viewportTransform->get_rect();
    float viewSize = (self->tableType == HMUI::TableView::TableType::Vertical) ? rect.get_height() : rect.get_width();
    float position = self->scrollView->get_position();

    self->scrollView->ScrollTo(std::min(position, std::max(self->cellSize * self->numberOfCells - viewSize, 0.0f)), false);
}

// override header cell behavior and change no data prefab
MAKE_HOOK_MATCH(LevelCollectionViewController_SetData, &LevelCollectionViewController::SetData,
        void, LevelCollectionViewController* self, IBeatmapLevelCollection* beatmapLevelCollection, StringW headerText, UnityEngine::Sprite* headerSprite, bool sortLevels, bool sortPreviewBeatmapLevels, UnityEngine::GameObject* noDataInfoPrefab) {
    // only check for null strings, not empty
    // string will be null for level collections but not level packs
    self->_showHeader = (bool) headerText;
    // copy base game method implementation
    self->_levelCollectionTableView->Init(headerText, headerSprite);
    self->_levelCollectionTableView->_showLevelPackHeader = self->_showHeader;
    if(self->_noDataInfoGO) {
        UnityEngine::Object::Destroy(self->_noDataInfoGO);
        self->_noDataInfoGO = nullptr;
    }
    // also override check for empty collection
    if(beatmapLevelCollection) {
        self->_levelCollectionTableView->get_gameObject()->SetActive(true);
        self->_levelCollectionTableView->SetData(beatmapLevelCollection->get_beatmapLevels(), self->_playerDataModel->playerData->favoritesLevelIds, sortLevels, sortPreviewBeatmapLevels);
        self->_levelCollectionTableView->RefreshLevelsAvailability();
    } else {
        if(noDataInfoPrefab)
            self->_noDataInfoGO = self->_container->InstantiatePrefab(noDataInfoPrefab, self->_noDataInfoContainer);
        // change no custom songs text if playlists exist
        // because if they do then the only way to get here with that specific no data indicator is to have no playlists filtered
        static ConstString message("No playlists are contained in the filtering options selected.");
        if(GetLoadedPlaylists().size() > 0 && noDataInfoPrefab == FindComponent<LevelFilteringNavigationController*>()->_emptyCustomSongListInfoPrefab.ptr())
            self->_noDataInfoGO->GetComponentInChildren<TMPro::TextMeshProUGUI*>()->set_text(message);
        self->_levelCollectionTableView->get_gameObject()->SetActive(false);
    }
    if(self->get_isInViewControllerHierarchy()) {
        if(self->_showHeader)
            self->_levelCollectionTableView->SelectLevelPackHeaderCell();
        else
            self->_levelCollectionTableView->ClearSelection();
        self->_songPreviewPlayer->CrossfadeToDefault();
    }
}

// make playlist selector only 5 playlists wide and add scrolling
MAKE_HOOK_MATCH(AnnotatedBeatmapLevelCollectionsGridView_OnEnable, &AnnotatedBeatmapLevelCollectionsGridView::OnEnable,
        void, AnnotatedBeatmapLevelCollectionsGridView* self) {

    self->GetComponent<UnityEngine::RectTransform*>()->set_anchorMax({0.83, 1});
    self->_pageControl->_content->get_gameObject()->SetActive(false);
    auto content = self->_animator->_contentTransform;
    content->set_anchoredPosition({0, content->get_anchoredPosition().y});
    
    AnnotatedBeatmapLevelCollectionsGridView_OnEnable(self);

    if(!self->GetComponent<Scroller*>())
        self->get_gameObject()->AddComponent<Scroller*>()->Init(self->_animator->_contentTransform);
}

// make the playlist opening animation work better with the playlist scroller
MAKE_HOOK_MATCH(AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen, &AnnotatedBeatmapLevelCollectionsGridViewAnimator::AnimateOpen,
        void, AnnotatedBeatmapLevelCollectionsGridViewAnimator* self, bool animated) {
    
    // store actual values to avoid breaking things when closing
    int rowCount = self->_rowCount;
    int selectedRow = self->_selectedRow;
    
    // lock height to specific value
    self->_rowCount = 5;
    self->_selectedRow = 0;

    AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen(self, animated);
    
    // prevent modification of content transform as it overrides the scroll view
    Tweening::Vector2Tween::getStaticF_Pool()->Despawn(self->_contentPositionTween);
    self->_contentPositionTween = nullptr;
    
    self->_rowCount = rowCount;
    self->_selectedRow = selectedRow;
}

// ensure animator doesn't get stuck at the wrong position
MAKE_HOOK_MATCH(AnnotatedBeatmapLevelCollectionsGridViewAnimator_ScrollToRowIdxInstant, &AnnotatedBeatmapLevelCollectionsGridViewAnimator::ScrollToRowIdxInstant,
        void, AnnotatedBeatmapLevelCollectionsGridViewAnimator* self, int selectedRow) {

    // despawns tweens and force sets the viewport and anchored pos
    self->AnimateClose(selectedRow, false);

    AnnotatedBeatmapLevelCollectionsGridViewAnimator_ScrollToRowIdxInstant(self, selectedRow);
}

// prevent download icon showing up on empty custom playlists unless manager is changing the behavior
MAKE_HOOK_MATCH(AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync, &AnnotatedBeatmapLevelCollectionCell::RefreshAvailabilityAsync,
        void, AnnotatedBeatmapLevelCollectionCell* self, IAdditionalContentModel* contentModel) {
    
    AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync(self, contentModel);

    if(hasManager)
        return;

    auto pack = il2cpp_utils::try_cast<IBeatmapLevelPack>(self->_annotatedBeatmapLevelCollection);
    if(pack.has_value()) {
        auto playlist = GetPlaylistWithPrefix(pack.value()->get_packID());
        if(playlist)
            self->SetDownloadIconVisible(false);
    }
}

// throw away objects on a soft restart
MAKE_HOOK_MATCH(MenuTransitionsHelper_RestartGame, &MenuTransitionsHelper::RestartGame,
        void, MenuTransitionsHelper* self, System::Action_1<Zenject::DiContainer*>* finishCallback) {

    for(auto scroller : UnityEngine::Resources::FindObjectsOfTypeAll<Scroller*>()) {
        UnityEngine::Object::Destroy(scroller);
    }

    ClearLoadedImages();

    hasLoaded = false;

    MenuTransitionsHelper_RestartGame(self, finishCallback);
    
    ResettableStaticPtr::resetAll();
}

// add our playlist selection view controller to the song downloader menu
MAKE_HOOK_FIND_CLASS_INSTANCE(DownloadSongsFlowCoordinator_DidActivate, "SongDownloader", "DownloadSongsFlowCoordinator", "DidActivate",
        void, HMUI::FlowCoordinator* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    
    DownloadSongsFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    if(firstActivation) {
        STATIC_AUTO(AddonViewController, SongDownloaderAddon::Create());
        self->_providedRightScreenViewController = AddonViewController;
    }
}

// add a playlist callback to the song downloader buttons
MAKE_HOOK_FIND_CLASS_INSTANCE(DownloadSongsSearchViewController_DidActivate, "SongDownloader", "DownloadSongsSearchViewController", "DidActivate",
        void, HMUI::ViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    
    DownloadSongsSearchViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    if(!firstActivation)
        return;

    using namespace UnityEngine;
    
    // add listeners to search entry buttons
    auto entryList = self->GetComponentsInChildren<UI::VerticalLayoutGroup*>()->First([](UI::VerticalLayoutGroup* x) {
        return x->get_name() == "BSMLScrollViewContentContainer";
    })->get_transform();

    // offset by one because the prefab doesn't seem to be destroyed yet here
    int entryCount = entryList->GetChildCount() - 1;
    for(int i = 0; i < entryCount; i++) {
        // undo offset to skip first element
        auto downloadButton = entryList->GetChild(i + 1)->GetComponentInChildren<UI::Button*>();
        // get entry at index with some lovely pointer addition
        SearchEntryHack* entryArrStart = (SearchEntryHack*) (((char*) self) + sizeof(HMUI::ViewController));
        // capture button array start and index
        downloadButton->get_onClick()->AddListener(custom_types::MakeDelegate<Events::UnityAction*>((std::function<void()>) [entryArrStart, i] {
            auto& entry = *(entryArrStart + i + 1);
            // get hash from entry
            std::string hash;
            if(entry.MapType == SearchEntryHack::MapType::BeatSaver)
                hash = entry.map.GetVersions().front().GetHash();
            else if(entry.MapType == SearchEntryHack::MapType::BeastSaber)
                hash = entry.BSsong.GetHash();
            else if(entry.MapType == SearchEntryHack::MapType::ScoreSaber)
                hash = entry.SSsong.GetId();
            // get json object from playlist
            auto playlist = SongDownloaderAddon::SelectedPlaylist;
            if(!playlist)
                return;
            auto& json = playlist->playlistJSON;
            // add a blank song
            json.Songs.emplace_back(BPSong());
            // set info
            auto& songJson = *(json.Songs.end() - 1);
            songJson.Hash = hash;
            // write to file
            playlist->Save();
            // have changes be updated
            MarkPlaylistForReload(playlist);
        }));
    }
}

// override to prevent crashes due to opening with a null level pack
#define COMBINE(delegate1, selfMethodName, ...) delegate1 = (__VA_ARGS__) System::Delegate::Combine(delegate1, System::Delegate::CreateDelegate(csTypeOf(__VA_ARGS__), self, #selfMethodName));
MAKE_HOOK_MATCH(LevelCollectionNavigationController_DidActivate, &LevelCollectionNavigationController::DidActivate,
        void, LevelCollectionNavigationController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {

    if(addedToHierarchy) {
        COMBINE(self->_levelCollectionViewController->didSelectLevelEvent, HandleLevelCollectionViewControllerDidSelectLevel, System::Action_2<UnityW<LevelCollectionViewController>, IPreviewBeatmapLevel*>*);
        COMBINE(self->_levelCollectionViewController->didSelectHeaderEvent, HandleLevelCollectionViewControllerDidSelectPack, System::Action_1<UnityW<LevelCollectionViewController>>*);
        COMBINE(self->_levelDetailViewController->didPressActionButtonEvent, HandleLevelDetailViewControllerDidPressActionButton, System::Action_1<UnityW<StandardLevelDetailViewController>>*);
        COMBINE(self->_levelDetailViewController->didPressPracticeButtonEvent, HandleLevelDetailViewControllerDidPressPracticeButton, System::Action_2<UnityW<StandardLevelDetailViewController>, IBeatmapLevel*>*);
        COMBINE(self->_levelDetailViewController->didChangeDifficultyBeatmapEvent, HandleLevelDetailViewControllerDidChangeDifficultyBeatmap, System::Action_2<UnityW<StandardLevelDetailViewController>, IDifficultyBeatmap*>*);
        COMBINE(self->_levelDetailViewController->didChangeContentEvent, HandleLevelDetailViewControllerDidPresentContent, System::Action_2<UnityW<StandardLevelDetailViewController>, StandardLevelDetailViewController::ContentType>*);
        COMBINE(self->_levelDetailViewController->didPressOpenLevelPackButtonEvent, HandleLevelDetailViewControllerDidPressOpenLevelPackButton, System::Action_2<UnityW<StandardLevelDetailViewController>, IBeatmapLevelPack*>*);
        COMBINE(self->_levelDetailViewController->levelFavoriteStatusDidChangeEvent, HandleLevelDetailViewControllerLevelFavoriteStatusDidChange, System::Action_2<UnityW<StandardLevelDetailViewController>, bool>*);
        if (self->_beatmapLevelToBeSelectedAfterPresent) {
            self->_levelCollectionViewController->SelectLevel(self->_beatmapLevelToBeSelectedAfterPresent);
            self->SetChildViewController(self->_levelCollectionViewController);
            self->_beatmapLevelToBeSelectedAfterPresent = nullptr;
        }
        else {
            // override here so that the pack detail controller will not be shown if no pack is selected
            if (self->_levelPack) {
                ArrayW<HMUI::ViewController*> children{2};
                children[0] = self->_levelCollectionViewController;
                children[1] = self->_levelPackDetailViewController;
                self->SetChildViewControllers(children);
            } else
                self->SetChildViewController(self->_levelCollectionViewController);
        }
    } else if(self->_loading) {
        self->ClearChildViewControllers();
    }
    else if(self->_hideDetailViewController) {
        self->PresentViewControllersForLevelCollection();
        self->_hideDetailViewController = false;
    }
}

extern "C" void setup(CModInfo* info) {
    info->id = "PlaylistCore";
    info->version = VERSION;
    info->version_long = 0;
    
    auto playlistsPath = GetPlaylistsPath();
    if(!direxists(playlistsPath))
        mkpath(playlistsPath);
    
    LOG_INFO("%s", playlistsPath.c_str());
    
    auto backupsPath = GetBackupsPath();
    if(!direxists(backupsPath))
        mkpath(backupsPath);
    
    auto coversPath = GetCoversPath();
    if(!direxists(coversPath))
        mkpath(coversPath);

    getConfig().Init(modInfo);
}

extern "C" void late_load() {
    LOG_INFO("Starting PlaylistCore installation...");
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    
    BSML::Init();
    BSML::Register::RegisterMenuButton("Reload Playlists", "Reloads all playlists!", []{ RuntimeSongLoader::API::RefreshSongs(false); });
    BSML::Register::RegisterSettingsMenu<SettingsViewController*>("Playlist Core");

    hasManager = modloader_require_mod(new CModInfo("PlaylistManager", VERSION, 0), CMatchType::MatchType_IdOnly);
    modloader_require_mod(new CModInfo("SongDownloader", VERSION, 0), CMatchType::MatchType_IdOnly);

    INSTALL_HOOK_ORIG(getLogger(), TableView_ReloadDataKeepingPosition);
    INSTALL_HOOK_ORIG(getLogger(), LevelCollectionViewController_SetData);
    INSTALL_HOOK(getLogger(), AnnotatedBeatmapLevelCollectionsGridView_OnEnable);
    INSTALL_HOOK(getLogger(), AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen);
    INSTALL_HOOK(getLogger(), AnnotatedBeatmapLevelCollectionsGridViewAnimator_ScrollToRowIdxInstant);
    INSTALL_HOOK(getLogger(), AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync);
    INSTALL_HOOK(getLogger(), MenuTransitionsHelper_RestartGame);
    //INSTALL_HOOK(getLogger(), DownloadSongsFlowCoordinator_DidActivate);
    //INSTALL_HOOK(getLogger(), DownloadSongsSearchViewController_DidActivate);
    INSTALL_HOOK_ORIG(getLogger(), LevelCollectionNavigationController_DidActivate);
    
    RuntimeSongLoader::API::AddRefreshLevelPacksEvent(
        [] (RuntimeSongLoader::SongLoaderBeatmapLevelPackCollectionSO* customBeatmapLevelPackCollectionSO) {
            LoadPlaylists(customBeatmapLevelPackCollectionSO, true);
        }
    );
    
    LOG_INFO("Successfully installed PlaylistCore!");
}
