/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITYCORE_GOSSIP_H
#define TRINITYCORE_GOSSIP_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Optional.h"

class Object;
class Quest;
class WorldSession;
struct GossipMenuItems;
enum class QuestGiverStatus : uint32;

#define GOSSIP_MAX_MENU_ITEMS               32
#define DEFAULT_GOSSIP_MESSAGE              0xffffff

enum class GossipOptionNpc : uint8
{
    None                       = 0,    // White chat bubble. Default
    Vendor                     = 1,    // Brown bag
    Taxinode                   = 2,    // White wing
    Trainer                    = 3,    // Brown book
    SpiritHealer               = 4,    // Golden interaction wheel (with red center)
    Binder                     = 5,    // Golden interaction wheel
    Banker                     = 6,    // Brown bag (with gold coin in lower corner)
    PetitionVendor             = 7,    // White chat bubble (with "..." inside)
    TabardVendor               = 8,    // White tabard
    Battlemaster               = 9,    // Two crossed swords
    Auctioneer                 = 10,   // Stack of gold coins
    TalentMaster               = 11,   // White chat bubble
    Stablemaster               = 12,   // White chat bubble
    PetSpecializationMaster    = 13,   /*DEPRECATED*/ // White chat bubble
    GuildBanker                = 14,   // White chat bubble
    Spellclick                 = 15,   // White chat bubble
    DisableXPGain              = 16,   // White chat bubble
    EnableXPGain               = 17,   // White chat bubble
    Mailbox                    = 18,   // White chat bubble
    WorldPvPQueue              = 19,   /*NYI*/ // White chat bubble
    LFGDungeon                 = 20,   /*NYI*/ // White chat bubble
    ArtifactRespec             = 21,   /*NYI*/ // White chat bubble
    CemeterySelect             = 22,   /*DEPRECATED*/ // White chat bubble
    SpecializationMaster       = 23,   /*DEPRECATED*/ // White chat bubble
    GlyphMaster                = 24,   /*DEPRECATED*/ // White chat bubble
    QueueScenario              = 25,   /*NYI*/ // White chat bubble
    GarrisonArchitect          = 26,   /*NYI*/ // White chat bubble
    GarrisonMissionNpc         = 27,   /*NYI*/ // White chat bubble
    ShipmentCrafter            = 28,   /*NYI*/ // Brown document
    GarrisonTradeskillNpc      = 29,   /*NYI*/ // White chat bubble
    GarrisonRecruitment        = 30,   /*NYI*/ // White chat bubble
    AdventureMap               = 31,   /*NYI*/ // White chat bubble
    GarrisonTalent             = 32,   // White chat bubble
    ContributionCollector      = 33,   /*NYI*/ // White chat bubble
    Transmogrify               = 34,   // Purple helm
    AzeriteRespec              = 35,   // White chat bubble
    IslandsMissionNpc          = 36,   /*NYI*/ // White chat bubble
    UIItemInteraction          = 37,   /*NYI*/ // White chat bubble
    WorldMap                   = 38,   /*NYI*/ // White chat bubble
    Soulbind                   = 39,   /*NYI*/ // White chat bubble
    ChromieTimeNpc             = 40,   /*NYI*/ // White chat bubble
    CovenantPreviewNpc         = 41,   /*NYI*/ // White chat bubble
    RuneforgeLegendaryCrafting = 42,   /*NYI*/ // White chat bubble
    NewPlayerGuide             = 43,   /*NYI*/ // White chat bubble
    RuneforgeLegendaryUpgrade  = 44,   /*NYI*/ // White chat bubble
    CovenantRenownNpc          = 45,   /*NYI*/ // White chat bubble
    BlackMarketAuctionHouse    = 46,
    PerksProgramVendor         = 47,
    ProfessionsCraftingOrder   = 48,
    ProfessionsOpen            = 49,
    ProfessionsCustomerOrder   = 50,
    TraitSystem                = 51,
    BarbersChoice              = 52,
    MajorFactionRenown         = 53,

    Count
};


struct GossipMenuItem
{
    GossipOptionNpc  OptionNpc;
    bool             IsCoded;
    std::string      Message;
    uint32           Sender;
    uint32           Action;
    std::string      BoxMessage;
    uint32           BoxMoney;
};

// need an ordered container
typedef std::map<uint32, GossipMenuItem> GossipMenuItemContainer;

struct GossipMenuItemData
{
    uint32 GossipActionMenuId;  // MenuId of the gossip triggered by this action
    uint32 GossipActionPoi;
};

// need an ordered container
typedef std::map<uint32, GossipMenuItemData> GossipMenuItemDataContainer;

struct QuestMenuItem
{
    uint32  QuestId;
    uint8   QuestIcon;
};

typedef std::vector<QuestMenuItem> QuestMenuItemList;

class TC_GAME_API GossipMenu
{
    public:
        GossipMenu();
        ~GossipMenu();

        uint32 AddMenuItem(int32 menuItemId, GossipOptionNpc optionNpc, std::string const& message, uint32 sender, uint32 action, std::string const& boxMessage, uint32 boxMoney, bool coded = false);
        void AddMenuItem(uint32 menuId, uint32 menuItemId, uint32 sender, uint32 action);

        void SetMenuId(uint32 menu_id) { _menuId = menu_id; }
        uint32 GetMenuId() const { return _menuId; }
        void SetLocale(LocaleConstant locale) { _locale = locale; }
        LocaleConstant GetLocale() const { return _locale; }

        void AddGossipMenuItemData(uint32 menuItemId, uint32 gossipActionMenuId, uint32 gossipActionPoi);

        uint32 GetMenuItemCount() const
        {
            return _menuItems.size();
        }

        bool Empty() const
        {
            return _menuItems.empty();
        }

        GossipMenuItem const* GetItem(uint32 id) const
        {
            GossipMenuItemContainer::const_iterator itr = _menuItems.find(id);
            if (itr != _menuItems.end())
                return &itr->second;

            return nullptr;
        }

        GossipMenuItemData const* GetItemData(uint32 indexId) const
        {
            GossipMenuItemDataContainer::const_iterator itr = _menuItemData.find(indexId);
            if (itr != _menuItemData.end())
                return &itr->second;

            return nullptr;
        }

        uint32 GetMenuItemSender(uint32 menuItemId) const;
        uint32 GetMenuItemAction(uint32 menuItemId) const;
        bool IsMenuItemCoded(uint32 menuItemId) const;

        void ClearMenu();

        GossipMenuItemContainer const& GetMenuItems() const
        {
            return _menuItems;
        }

    private:
        GossipMenuItemContainer _menuItems;
        GossipMenuItemDataContainer _menuItemData;
        uint32 _menuId;
        LocaleConstant _locale;
};

class TC_GAME_API QuestMenu
{
    public:
        QuestMenu();
        QuestMenu(QuestMenu const&) = delete;
        QuestMenu(QuestMenu&&) = delete;
        QuestMenu& operator=(QuestMenu const&) = delete;
        QuestMenu& operator=(QuestMenu&&) = delete;
        ~QuestMenu();

        void AddMenuItem(uint32 QuestId, uint8 Icon);
        void ClearMenu();
        uint8 GetMenuItemCount() const{ return uint8(_questMenuItems.size()); }
        bool Empty() const { return _questMenuItems.empty(); }
        bool HasItem(uint32 questId) const;
        QuestMenuItem const& GetItem(uint16 index) const { return _questMenuItems[index]; }

    private:
        QuestMenuItemList _questMenuItems;
};

class InteractionData
{
    public:
        void Reset()
        {
            SourceGuid.Clear();
            TrainerId = 0;
            PlayerChoiceId = 0;
        }

        ObjectGuid SourceGuid;
        uint32 TrainerId = 0;
        uint32 PlayerChoiceId = 0;
};

class TC_GAME_API PlayerMenu
{
    public:
        explicit PlayerMenu(WorldSession* session);
        PlayerMenu(PlayerMenu const&) = delete;
        PlayerMenu(PlayerMenu&&) = delete;
        PlayerMenu& operator=(PlayerMenu const&) = delete;
        PlayerMenu& operator=(PlayerMenu&&) = delete;
        ~PlayerMenu();

        GossipMenu& GetGossipMenu() { return _gossipMenu; }
        QuestMenu& GetQuestMenu() { return _questMenu; }
        InteractionData& GetInteractionData() { return _interactionData; }

        bool Empty() const { return _gossipMenu.Empty() && _questMenu.Empty(); }

        void ClearMenus();
        uint32 GetGossipOptionSender(uint32 selection) const { return _gossipMenu.GetMenuItemSender(selection); }
        uint32 GetGossipOptionAction(uint32 selection) const { return _gossipMenu.GetMenuItemAction(selection); }
        bool IsGossipOptionCoded(uint32 selection) const { return _gossipMenu.IsMenuItemCoded(selection); }

        void SendGossipMenu(uint32 titleTextId, ObjectGuid objectGUID);
        void SendCloseGossip();
        void SendPointOfInterest(uint32 poiId) const;

        /*********************************************************/
        /***                    QUEST SYSTEM                   ***/
        /*********************************************************/
        void SendQuestGiverStatus(QuestGiverStatus questStatus, ObjectGuid npcGUID) const;

        void SendQuestGiverQuestListMessage(Object* questgiver);

        void SendQuestQueryResponse(Quest const* quest) const;
        void SendQuestGiverQuestDetails(Quest const* quest, ObjectGuid npcGUID, bool autoLaunched, bool displayPopup) const;

        void SendQuestGiverOfferReward(Quest const* quest, ObjectGuid npcGUID, bool autoLaunched) const;
        void SendQuestGiverRequestItems(Quest const* quest, ObjectGuid npcGUID, bool canComplete, bool autoLaunched) const;

    private:
        GossipMenu _gossipMenu;
        QuestMenu  _questMenu;
        WorldSession* _session;
        InteractionData _interactionData;
};
#endif
