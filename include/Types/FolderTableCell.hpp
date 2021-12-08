#pragma once

#include "Types/CustomListSource.hpp"

#include "HMUI/ImageView.hpp"
#include "TMPro/TextMeshProUGUI.hpp"

DECLARE_CLASS_CUSTOM(PlaylistManager, FolderTableCell, PlaylistManager::CustomTableCell,
    
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, hoverImage);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, selectImage);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, folderName);

    DECLARE_CTOR(ctor);
    void refreshVisuals();
    void init(UnityEngine::Sprite* sprite, std::string text);
    void setText(std::string text);
)