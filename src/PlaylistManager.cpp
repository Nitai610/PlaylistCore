#include "Main.hpp"
#include "Types/BPList.hpp"
#include "Types/Config.hpp"
#include "PlaylistManager.hpp"
#include "ResettableStaticPtr.hpp"

#include <filesystem>
#include <fstream>

#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "questui/shared/BeatSaberUI.hpp"

#include "songloader/shared/API.hpp"

#include "System/Convert.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "GlobalNamespace/CustomLevelLoader.hpp"

using namespace RuntimeSongLoader;

std::string SanitizeFileName(std::string fileName) {
    std::string newName;
    // yes I know not all of these are disallowed, and also that they are unlikely to end up in a name
    static const std::unordered_set<unsigned char> replacedChars = {
        '/', '\n', '\\', '\'', '\"', ' ', '?', '<', '>', ':', '*'
    };
    std::transform(fileName.begin(), fileName.end(), std::back_inserter(newName), [](unsigned char c){
        if(replacedChars.contains(c))
            return (unsigned char)('_');
        return c;
    });
    if(newName == "")
        return "_";
    return newName;
}

bool UniqueFileName(std::string fileName, std::string compareDirectory) {
    if(!std::filesystem::is_directory(compareDirectory))
        return true;
    for(auto& entry : std::filesystem::directory_iterator(compareDirectory)) {
        if(entry.is_directory())
            continue;
        if(entry.path().filename().string() == fileName)
            return false;
    }
    return true;
}

bool ShouldAddPack(std::string name) {
    if(currentFolder && folderSelectionState == 3) {
        for(std::string testName : currentFolder->PlaylistNames) {
            if(name == testName)
                return true;
        }
    }
    return folderSelectionState == 0 || folderSelectionState == 2;
}

std::string GetLevelHash(GlobalNamespace::CustomPreviewBeatmapLevel* level) {
    std::string id = STR(level->levelID);
    // should be in all songloader levels
    auto prefixIndex = id.find("custom_level_");
    if(prefixIndex == std::string::npos)
        return "";
    // remove prefix
    id = id.substr(prefixIndex + 14);
    auto wipIndex = id.find(" WIP");
    if(wipIndex != std::string::npos)
        id = id.substr(0, wipIndex);
    std::transform(id.begin(), id.end(), id.begin(), toupper);
    return id;
}

namespace PlaylistManager {
    
    std::unordered_map<std::string, Playlist*> name_playlists;
    std::unordered_map<std::string, Playlist*> path_playlists;

    // map from playlist names to json objects
    // std::unordered_map<std::string, BPList*> playlists_json;
    // map from file paths to songloader playlists
    // SafePtr<System::Collections::Generic::Dictionary_2<Il2CppString*, SongLoaderCustomBeatmapLevelPack*>> path_playlists;
    // keep track of images to avoid loading duplicates
    std::hash<std::string> hasher;
    std::unordered_map<std::size_t, int> imageHashes;
    
    // array of all loaded images
    std::vector<UnityEngine::Sprite*> loadedImages;
    // all unusable playlists
    const std::unordered_set<std::string> staticPacks = {
        "Original Soundtrack Vol. 1",
        "Original Soundtrack Vol. 2",
        "Original Soundtrack Vol. 3",
        "Original Soundtrack Vol. 4",
        "Extras",
        "Camellia Music Pack",
        "Skrillex Music Pack",
        "Interscope Mixtape",
        "BTS Music Pack",
        "Linkin Park Music Pack",
        "Timbaland Music Pack",
        "Green Day Music Pack",
        "Rocket League x Monstercat Music Pack",
        "Panic! At The Disco Music Pack",
        "Imagine Dragons Music Pack",
        "Monstercat Music Pack Vol. 1",
        "Custom Levels",
        "WIP Levels"
    };
    
    std::string GetBase64ImageType(std::string& base64) {
        if(base64.length() < 3)
            return "";
        std::string sub = base64.substr(0, 3);
        if(sub == "iVB")
            return ".png";
        if(sub == "/9j")
            return ".jpg";
        if(sub == "R0l")
            return ".gif";
        if(sub == "Qk1")
            return ".bmp";
        return "";
    }

    // returns base 64 string of png as well
    std::string WriteImageToFile(std::string_view pathToPng, UnityEngine::Sprite* image) {
        auto bytes = UnityEngine::ImageConversion::EncodeToPNG(image->get_texture());
        writefile(pathToPng, std::string(reinterpret_cast<char*>(bytes.begin()), bytes.Length()));
        return STR(System::Convert::ToBase64String(bytes));
    }
    
    int GetPackIndex(std::string title) {
        // find index of playlist title in config
        int size = playlistConfig.Order.size();
        for(int i = 0; i < size; i++) {
            if(playlistConfig.Order[i] == title)
                return i;
        }
        // add to end of config if not found
        playlistConfig.Order.push_back(title);
        return -1;
    }
    
    UnityEngine::Sprite* GetDefaultCoverImage() {
        STATIC_AUTO(defaultCover, UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::CustomLevelLoader*>()[0]->defaultPackCover);
        return defaultCover;
    }

    UnityEngine::Sprite* GetCoverImage(Playlist* playlist) {
        auto& json = playlist->playlistJSON;
        if(json.ImageString.has_value()) {
            std::string imageBase64 = json.ImageString.value();
            // trim "data:image/png;base64,"-like metadata
            static std::string searchString = "base64,";
            auto searchIndex = imageBase64.find(searchString);
            if(searchIndex != std::string::npos)
                imageBase64 = imageBase64.substr(searchIndex + searchString.length());
            // return loaded image if existing
            std::size_t imgHash = hasher(imageBase64);
            auto foundItr = imageHashes.find(imgHash);
            if (foundItr != imageHashes.end()) {
                LOG_INFO("Returning loaded image with hash %lu", imgHash);
                playlist->imageIndex = foundItr->second;
                return loadedImages[foundItr->second];
            }
            // check image type
            std::string imgType = GetBase64ImageType(imageBase64);
            if(imgType != ".png" && imgType != ".jpg") {
                LOG_ERROR("Unsupported image type %s", imgType.c_str());
                return GetDefaultCoverImage();
            }
            // get and write sprite
            auto sprite = QuestUI::BeatSaberUI::Base64ToSprite(imageBase64);
            // compare sanitized hash
            // can sometimes need two passes?? idk but I don't want to add to the lag and do it on every playlist, so just restart I guess
            auto bytes = UnityEngine::ImageConversion::EncodeToPNG(sprite->get_texture());
            std::size_t oldHash = imgHash;
            imageBase64 = STR(System::Convert::ToBase64String(bytes));
            imgHash = hasher(imageBase64);
            // write to playlist if changed
            if(imgHash != oldHash) {
                json.ImageString = imageBase64;
                WriteToFile(playlist->path, json);
            }
            foundItr = imageHashes.find(imgHash);
            if (foundItr != imageHashes.end()) {
                LOG_INFO("Returning loaded image with hash %lu", imgHash);
                playlist->imageIndex = foundItr->second;
                return loadedImages[foundItr->second];
            }
            // save image as file and return
            LOG_INFO("Writing image with hash %lu", imgHash);
            std::string imgPath = GetCoversPath() + "/" + playlist->name + "_" + std::to_string(imgHash) + ".png";
            writefile(imgPath, std::string(reinterpret_cast<char*>(bytes.begin()), bytes.Length()));
            imageHashes.insert({imgHash, loadedImages.size()});
            playlist->imageIndex = loadedImages.size();
            loadedImages.emplace_back(sprite);
            return sprite;
        }
        return GetDefaultCoverImage();
    }

    void GetCoverImages() {
        // ensure path exists
        auto path = GetCoversPath();
        if(!std::filesystem::is_directory(path))
            return;
        // iterate through all image files
        for(const auto& file : std::filesystem::directory_iterator(path)) {
            if(!file.is_directory()) {
                auto path = file.path();
                // check file extension
                if(path.extension().string() == ".jpg") {
                    auto newPath = path.parent_path() / (path.stem().string() + ".png");
                    std::filesystem::rename(path, newPath);
                    path = newPath;
                } else if(path.extension().string() != ".png") {
                    LOG_ERROR("Incompatible file extension: %s", path.extension().string().c_str());
                    continue;
                }
                // check hash of base image before converting to sprite and to png
                std::ifstream instream(path, std::ios::in | std::ios::binary | std::ios::ate);
                auto size = instream.tellg();
                instream.seekg(0, instream.beg);
                auto bytes = Array<uint8_t>::NewLength(size);
                instream.read(reinterpret_cast<char*>(bytes->values), size);
                std::size_t imgHash = hasher(STR(System::Convert::ToBase64String(bytes)));
                if(imageHashes.contains(imgHash)) {
                    LOG_INFO("Skipping loading image with hash %lu", imgHash);
                    continue;
                }
                // sanatize hash by converting to png
                auto sprite = QuestUI::BeatSaberUI::ArrayToSprite(bytes);
                imgHash = hasher(WriteImageToFile(path.string(), sprite));
                // check hash with loaded images
                if(imageHashes.contains(imgHash)) {
                    LOG_INFO("Skipping loading image with hash %lu", imgHash);
                    continue;
                }
                LOG_INFO("Loading image with hash %lu", imgHash);
                // rename file with hash for identification (coolImage -> coolImage_1987238271398)
                auto searchIndex = path.stem().string().rfind("_") + 1;
                if(path.stem().string().substr(searchIndex) != std::to_string(imgHash)) {
                    LOG_INFO("Renaming image file with hash %lu", imgHash);
                    std::filesystem::rename(path, path.parent_path() / (path.stem().string() + "_" + std::to_string(imgHash) + ".png"));
                }
                imageHashes.insert({imgHash, loadedImages.size()});
                loadedImages.emplace_back(sprite);
            }
        }
    }

    std::vector<UnityEngine::Sprite*>& GetLoadedImages() {
        return loadedImages;
    }

    void ClearLoadedImages() {
        loadedImages.clear();
        imageHashes.clear();
    }

    bool AvailablePlaylistName(std::string title) {
        // check in constant playlists
        if(staticPacks.contains(title))
            return false;
        // check in custom playlists
        for(auto& pair : name_playlists) {
            if(pair.second->name == title)
                return false;
        }
        return true;
    }

    void LoadPlaylists(SongLoaderBeatmapLevelPackCollectionSO* customBeatmapLevelPackCollectionSO, bool fullRefresh) {
        // load images if none are loaded
        if(loadedImages.size() < 1)
            GetCoverImages();
        // clear playlists if requested
        if(fullRefresh) {
            for(auto& pair : name_playlists)
                delete pair.second;
            name_playlists.clear();
            path_playlists.clear();
        }
        // ensure path exists
        auto path = GetPlaylistsPath();
        if(!std::filesystem::is_directory(path))
            return;
        // create array for playlists
        std::vector<GlobalNamespace::CustomBeatmapLevelPack*> sortedPlaylists(playlistConfig.Order.size());
        // iterate through all playlist files
        for(const auto& entry : std::filesystem::directory_iterator(path)) {
            if(!entry.is_directory()) {
                // check if playlist has been loaded already
                auto path = entry.path().string();
                auto iter = path_playlists.find(path);
                if(iter != path_playlists.end()) {
                    LOG_INFO("Loading playlist file %s from cache", path.c_str());
                    // check if playlist should be added
                    auto playlist = iter->second;
                    if(ShouldAddPack(playlist->name)) {
                        int packPosition = GetPackIndex(playlist->name);
                        // add if new (idk how)
                        if(packPosition < 0)
                            sortedPlaylists.emplace_back(playlist->playlistCS);
                        else
                            sortedPlaylists[packPosition] = playlist->playlistCS;
                    }
                } else {
                    LOG_INFO("Loading playlist file %s", path.c_str());
                    // get playlist object from file
                    Playlist* playlist = new Playlist();
                    if(ReadFromFile(path, playlist->playlistJSON)) {
                        playlist->name = playlist->playlistJSON.PlaylistTitle;
                        playlist->path = path;
                        // check for duplicate name
                        while(name_playlists.find(playlist->name) != name_playlists.end()) {
                            LOG_INFO("Duplicate playlist name!");
                            RenamePlaylist(playlist, playlist->name + ".");
                            path = playlist->path;
                        }
                        name_playlists.insert({playlist->name, playlist});
                        path_playlists.insert({playlist->path, playlist});
                        // create playlist object
                        SongLoaderCustomBeatmapLevelPack* songloaderBeatmapLevelPack = SongLoaderCustomBeatmapLevelPack::New_ctor(playlist->name, playlist->name, GetCoverImage(playlist));
                        playlist->playlistCS = songloaderBeatmapLevelPack->CustomLevelsPack;
                        // add all songs to the playlist object
                        auto foundSongs = List<GlobalNamespace::CustomPreviewBeatmapLevel*>::New_ctor();
                        for(auto& song : playlist->playlistJSON.Songs) {
                            auto search = RuntimeSongLoader::API::GetLevelByHash(song.Hash);
                            if(search.has_value())
                                foundSongs->Add(search.value());
                        }
                        songloaderBeatmapLevelPack->SetCustomPreviewBeatmapLevels(foundSongs->ToArray());
                        // add the playlist to the sorted array
                        if(ShouldAddPack(playlist->name)) {
                            int packPosition = GetPackIndex(playlist->name);
                            // add if new
                            if(packPosition < 0)
                                sortedPlaylists.emplace_back(songloaderBeatmapLevelPack->CustomLevelsPack);
                            else
                                sortedPlaylists[packPosition] = songloaderBeatmapLevelPack->CustomLevelsPack;
                        }
                    } else
                        delete playlist;
                }
            }
        }
        // add playlists to game in sorted order
        for(auto customBeatmapLevelPack : sortedPlaylists) {
            if(customBeatmapLevelPack)
                customBeatmapLevelPackCollectionSO->AddLevelPack(customBeatmapLevelPack);
        }
    }

    std::vector<GlobalNamespace::CustomBeatmapLevelPack*> GetLoadedPlaylists() {
        // create return vector with base size
        std::vector<GlobalNamespace::CustomBeatmapLevelPack*> ret(playlistConfig.Order.size());
        for(auto& pair : name_playlists) {
            auto& playlist = pair.second;
            auto pack = playlist->playlistCS;
            if(pack) {
                int idx = GetPackIndex(playlist->name);
                if(idx >= 0)
                    ret[idx] = pack;
                else
                    ret.push_back(pack);
            }
        }
        // remove empty slots
        int i = 0;
        for(auto itr = ret.begin(); itr != ret.end(); itr++) {
            if(*itr == nullptr) {
                ret.erase(itr);
                itr--;
            }
            i++;
        }
        return ret;
    }

    Playlist* GetPlaylist(std::string title) {
        auto iter = name_playlists.find(title);
        if(iter == name_playlists.end())
            return nullptr;
        return iter->second;
    }

    void AddPlaylist(std::string title, std::string author, UnityEngine::Sprite* coverImage) {
        // create playlist with info
        auto newPlaylist = BPList();
        newPlaylist.PlaylistTitle = title;
        if(author != "")
            newPlaylist.PlaylistAuthor = author;
        auto bytes = UnityEngine::ImageConversion::EncodeToPNG(coverImage->get_texture());
        newPlaylist.ImageString = STR(System::Convert::ToBase64String(bytes));
        // save playlist
        std::string fileTitle = SanitizeFileName(title);
        while(!UniqueFileName(fileTitle + ".bplist_BMBF.json", GetPlaylistsPath()))
            fileTitle += "_";
        WriteToFile(GetPlaylistsPath() + "/" + fileTitle + ".bplist_BMBF.json", newPlaylist);
        RefreshPlaylists(); // load it, with the others because I'm lazy
    }

    void MovePlaylist(Playlist* playlist, int index) {
        int originalIndex = GetPackIndex(playlist->name);
        if(originalIndex < 0) {
            LOG_ERROR("Attempting to move unloaded playlist");
            return;
        }
        playlistConfig.Order.erase(playlistConfig.Order.begin() + originalIndex);
        playlistConfig.Order.insert(playlistConfig.Order.begin() + index, playlist->name);
        WriteToFile(GetConfigPath(), playlistConfig);
    }

    void RenamePlaylist(Playlist* playlist, std::string title) {
        std::string oldName = playlist->name;
        std::string oldPath = playlist->path;
        int orderIndex = GetPackIndex(playlist->name);
        if(orderIndex < 0) {
            LOG_ERROR("Attempting to rename unloaded playlist");
            return;
        }
        // update name in map
        auto name_iter = name_playlists.find(playlist->name);
        if(name_iter == name_playlists.end()) {
            LOG_ERROR("Could not find playlist by name");
            return;
        }
        // also update path
        auto path_iter = path_playlists.find(playlist->name);
        if(path_iter == path_playlists.end()) {
            LOG_ERROR("Could not find playlist by path");
            return;
        }
        std::string fileTitle = SanitizeFileName(title);
        while(!UniqueFileName(fileTitle + ".bplist_BMBF.json", GetPlaylistsPath()))
            fileTitle += "_";
        std::string newPath = GetPlaylistsPath() + "/" + fileTitle + ".bplist_BMBF.json";
        // edit variables
        playlistConfig.Order[orderIndex] = title;
        playlist->name = title;
        playlist->playlistJSON.PlaylistTitle = title;
        name_playlists.erase(name_iter);
        name_playlists.insert({title, playlist});
        path_playlists.erase(path_iter);
        path_playlists.insert({newPath, playlist});
        // change name in all folders
        for(auto& folder : playlistConfig.Folders) {
            for(int i = 0; i < folder.PlaylistNames.size(); i++) {
                if(folder.PlaylistNames[i] == oldName)
                    folder.PlaylistNames[i] = title;
            }
        }
        // rename playlist ingame
        auto levelPack = playlist->playlistCS;
        if(levelPack) {
            auto nameCS = CSTR(playlist->name);
            levelPack->packName = nameCS;
            levelPack->shortPackName = nameCS;
        }
        // save changes
        WriteToFile(GetConfigPath(), playlistConfig);
        WriteToFile(oldPath, playlist->playlistJSON);
        // rename file
        std::filesystem::rename(oldPath, newPath);
        playlist->path = newPath;
    }

    void ChangePlaylistCover(Playlist* playlist, UnityEngine::Sprite* newCover, int coverIndex) {
        // don't save string for default cover
        bool isDefault = false;
        if(!newCover) {
            LOG_INFO("Changing playlist cover to default");
            newCover = GetDefaultCoverImage();
            isDefault = true;
        }
        playlist->imageIndex = coverIndex;
        auto json = playlist->playlistJSON;
        if(isDefault)
            json.ImageString = std::nullopt;
        else {
            LOG_INFO("Changing playlist cover");
            // save image base 64
            auto bytes = UnityEngine::ImageConversion::EncodeToPNG(newCover->get_texture());
            json.ImageString = STR(System::Convert::ToBase64String(bytes));
        }
        // change cover ingame
        auto levelPack = playlist->playlistCS;
        if(levelPack) {
            levelPack->coverImage = newCover;
            levelPack->smallCoverImage = newCover;
        }
        WriteToFile(playlist->path, json);
    }

    void DeletePlaylist(Playlist* playlist) {
        // remove from maps
        auto name_iter = name_playlists.find(playlist->name);
        if(name_iter == name_playlists.end()) {
            LOG_ERROR("Could not find playlist by name");
            return;
        }
        auto path_iter = path_playlists.find(playlist->path);
        if(path_iter == path_playlists.end()) {
            LOG_ERROR("Could not find playlist by path");
            return;
        }
        name_playlists.erase(name_iter);
        path_playlists.erase(path_iter);
        // delete file
        std::filesystem::remove(playlist->path);
        // remove name from order config
        int orderIndex = GetPackIndex(playlist->name);
        if(orderIndex >= 0)
            playlistConfig.Order.erase(playlistConfig.Order.begin() + orderIndex);
        else
            playlistConfig.Order.erase(playlistConfig.Order.end() - 1);
        // delete playlist object
        delete playlist;
        // reload because I am again lazy
        RefreshPlaylists();
    }

    void RefreshPlaylists() {
        bool showDefaults = folderSelectionState == 0 || folderSelectionState == 1;
        if(folderSelectionState == 3 && currentFolder)
            showDefaults = currentFolder->ShowDefaults;
        API::RefreshPacks(showDefaults);
    }

    void AddSongToPlaylist(Playlist* playlist, GlobalNamespace::CustomPreviewBeatmapLevel* level) {
        auto json = playlist->playlistJSON;
        // add a blank song
        json.Songs.emplace_back(BPSong());
        // set info
        auto& songJson = *(json.Songs.end() - 1);
        songJson.Hash = GetLevelHash(level);
        songJson.SongName = STR(level->songName);
        // write to file
        WriteToFile(playlist->path, json);
    }

    void RemoveSongFromPlaylist(Playlist* playlist, GlobalNamespace::CustomPreviewBeatmapLevel* level) {
        auto json = playlist->playlistJSON;
        // find song by hash (since the field is required) and remove
        auto levelHash = GetLevelHash(level);
        for(auto itr = json.Songs.begin(); itr != json.Songs.end(); ++itr) {
            auto& song = *itr;
            std::transform(song.Hash.begin(), song.Hash.end(), song.Hash.begin(), toupper);
            std::transform(levelHash.begin(), levelHash.end(), levelHash.begin(), toupper);
            if(song.Hash == levelHash) {
                json.Songs.erase(itr);
                // only erase
                break;
            }
        }
        // write to file
        WriteToFile(playlist->path, json);
    }
}
