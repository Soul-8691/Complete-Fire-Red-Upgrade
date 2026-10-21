#include "../include/global.h"
#include "../include/gflib.h"
#include "../include/main.h"
#include "../include/menu.h"
#include "../include/new_menu_helpers.h"
#include "../include/task.h"
#include "../include/scanline_effect.h"
#include "../include/overworld.h"
#include "../include/fieldmap.h"
#include "../include/global.fieldmap.h"
#include "../include/event_data.h"
#include "../include/event_object_movement.h"
#include "../include/region_map.h"
#include "../include/wild_encounter.h"
#include "../include/pokemon_icon.h"
#include "../include/characters.h"
#include "../include/battle.h"
#include "../include/data.h"
#include "../include/item.h"
#include "../include/pokemon.h"
#include "../include/map_viewer.h"
#include "../include/constants/songs.h"
#include "../include/constants/rgb.h"
#include "../include/constants/event_object_movement.h"
#include "../include/constants/trainer_types.h"
#include "../include/constants/trainers.h"
#include "../include/constants/battle_setup.h"
#include "../include/constants/map_scripts.h"
#include "../include/constants/items.h"
#include "../include/constants/moves.h"

// From constants/event_objects.h and constants/maps.h, which can't be included here
// (they need the generated map_event_ids.h and map_groups.h)
#define OBJ_EVENT_GFX_VARS 240
#define OBJ_KIND_CLONE     255
#define MAP_DYNAMIC_GROUP  0x7F // MAP_DYNAMIC: a warp back to wherever the player came from
#define MAP_DYNAMIC_NUM    0x7F
#define MAP_UNDEFINED_GROUP 0xFF // Ends gWildMonHeaders
// Where FireRed keeps the rival's name. CFRU's struct SaveBlock1 doesn't match the ROM's layout here.
#define SAVEBLOCK1_RIVAL_NAME_OFFSET 0x3A4C

// Viewer that draws a map's layout and object events (as seen in a map editor).
// D-Pad moves the cursor (the view follows it), B fades out and switches to the return callback.
// When the cursor is on a warp, the map it leads to is shown in a text box, and A opens that map
// with the cursor on the warp's target.
// Holding L shows the map's wild encounter slots, one page per encounter method (D-Pad changes page).

#define MAP_VIEWER_WIDTH  15 // Screen size in metatiles
#define MAP_VIEWER_HEIGHT 10
#define MAP_VIEWER_TILEMAP_SIZE (32 * 32 * sizeof(u16))

// Same layer arrangement as the overworld (see sOverworldBgTemplates).
// All three share char block 0, which is where CopyMapTilesetsToVram puts the tileset (via BG2).
#define BG_TOP    1
#define BG_MIDDLE 2
#define BG_BOTTOM 3
// Translucent highlights and the warp info box, over everything. Uses the free char block 2 and map base 28.
#define BG_OVERLAY 0

#define HIGHLIGHT_TILE 1 // Tile 0 is left blank (transparent)
#define PALSLOT_HIGHLIGHT_WARP    13 // The tilesets only use palettes 0-12
#define PALSLOT_HIGHLIGHT_TRAINER 14
#define PALSLOT_INFO_TEXT         15

// Everything handed to the ROM's own functions is ALIGNED(4): the ROM was built with agbcc, which aligns
// every struct to 4 bytes and copies them a word at a time, but this compiler only aligns them to their
// largest member. A template that lands on a 2-byte boundary gets read as garbage.
static const u32 ALIGNED(4) sHighlightTile[8] =
{
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
};

static const u16 ALIGNED(4) sHighlightWarpColor = RGB(31, 31, 0);
static const u16 ALIGNED(4) sHighlightTrainerColor = RGB(31, 0, 0);

// 0: transparent, 1: box background, 2: text, 3: text shadow
static const u16 ALIGNED(4) sInfoTextPal[] = {RGB_BLACK, RGB(2, 2, 4), RGB_WHITE, RGB(11, 11, 13)};
static const u8 sInfoTextColors[] = {1, 2, 3};

static const struct BgTemplate ALIGNED(4) sMapViewerBgTemplates[] =
{
    {
        .bg = BG_OVERLAY,
        .charBaseIndex = 2,
        .mapBaseIndex = 28,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0,
    },
    {
        .bg = BG_TOP,
        .charBaseIndex = 0,
        .mapBaseIndex = 29,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 1,
        .baseTile = 0,
    },
    {
        .bg = BG_MIDDLE,
        .charBaseIndex = 0,
        .mapBaseIndex = 30,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 2,
        .baseTile = 0,
    },
    {
        .bg = BG_BOTTOM,
        .charBaseIndex = 0,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 3,
        .baseTile = 0,
    },
};

// The info box goes on whichever half of the screen the cursor isn't on
enum
{
    WIN_INFO_TOP,
    WIN_INFO_BOTTOM,
    WIN_PANEL, // Full screen. Never shown at the same time as the info box, so it reuses its tiles.
    WIN_INFO_NONE = 0xFF,
};

#define INFO_WINDOW_HEIGHT 4 // In tiles

static const struct WindowTemplate ALIGNED(4) sMapViewerWindowTemplates[] =
{
    [WIN_INFO_TOP] =
    {
        .bg = BG_OVERLAY,
        .tilemapLeft = 0,
        .tilemapTop = 0,
        .width = 30,
        .height = INFO_WINDOW_HEIGHT,
        .paletteNum = PALSLOT_INFO_TEXT,
        .baseBlock = HIGHLIGHT_TILE + 1,
    },
    [WIN_INFO_BOTTOM] =
    {
        .bg = BG_OVERLAY,
        .tilemapLeft = 0,
        .tilemapTop = 20 - INFO_WINDOW_HEIGHT,
        .width = 30,
        .height = INFO_WINDOW_HEIGHT,
        .paletteNum = PALSLOT_INFO_TEXT,
        .baseBlock = HIGHLIGHT_TILE + 1 + 30 * INFO_WINDOW_HEIGHT,
    },
    [WIN_PANEL] =
    {
        .bg = BG_OVERLAY,
        .tilemapLeft = 0,
        .tilemapTop = 0,
        .width = 30,
        .height = 20,
        .paletteNum = PALSLOT_INFO_TEXT,
        .baseBlock = HIGHLIGHT_TILE + 1, // Tiles 2-601, which spill into the free start of char block 3
    },
    DUMMY_WIN_TEMPLATE
};

// Wild encounter panel: a header line, then a 3x4 grid with one cell per encounter slot
enum
{
    WILD_METHOD_LAND,
    WILD_METHOD_WATER,
    WILD_METHOD_ROCK_SMASH,
    WILD_METHOD_OLD_ROD,
    WILD_METHOD_GOOD_ROD,
    WILD_METHOD_SUPER_ROD,
    WILD_METHOD_COUNT,
};

#define WILD_PANEL_MAX_SLOTS  LAND_WILD_COUNT
#define WILD_PANEL_COLUMNS    3
#define WILD_PANEL_HEADER_H   16
#define WILD_PANEL_CELL_W     80
#define WILD_PANEL_CELL_H     36

// Chance of each slot being picked, in %. Same as the tables in wild_encounter.c
static const u8 sLandSlotRates[LAND_WILD_COUNT] = {20, 20, 10, 10, 10, 10, 5, 5, 4, 4, 1, 1};
static const u8 sWaterSlotRates[WATER_WILD_COUNT] = {60, 30, 5, 4, 1};
static const u8 sFishingSlotRates[FISH_WILD_COUNT] = {70, 30, 60, 20, 20, 40, 40, 15, 4, 1}; // Old, Good, Super Rod

struct WildMethod
{
    const u8 *name;
    u8 firstSlot; // The rods share the fishing table
    u8 numSlots;
    const u8 *slotRates;
};

extern const u8 gText_MapViewer_Land[];
extern const u8 gText_MapViewer_Water[];
extern const u8 gText_MapViewer_RockSmash[];
extern const u8 gText_MapViewer_OldRod[];
extern const u8 gText_MapViewer_GoodRod[];
extern const u8 gText_MapViewer_SuperRod[];

static const struct WildMethod sWildMethods[WILD_METHOD_COUNT] =
{
    [WILD_METHOD_LAND]       = {gText_MapViewer_Land,      0, LAND_WILD_COUNT,  sLandSlotRates},
    [WILD_METHOD_WATER]      = {gText_MapViewer_Water,     0, WATER_WILD_COUNT, sWaterSlotRates},
    [WILD_METHOD_ROCK_SMASH] = {gText_MapViewer_RockSmash, 0, ROCK_WILD_COUNT,  sWaterSlotRates},
    [WILD_METHOD_OLD_ROD]    = {gText_MapViewer_OldRod,    0, 2,                sFishingSlotRates},
    [WILD_METHOD_GOOD_ROD]   = {gText_MapViewer_GoodRod,   2, 3,                sFishingSlotRates + 2},
    [WILD_METHOD_SUPER_ROD]  = {gText_MapViewer_SuperRod,  5, 5,                sFishingSlotRates + 5},
};

static const u8 sPanelTextColors[] = {1, 2, 3};

// Trainer party panel (hold R on a trainer): a header line, then a 2x3 grid with one cell per party member
// Trainer party panel: a header line, then one full width row per party member, 3 per page (Up/Down changes page)
#define TRAINER_PANEL_ROWS    3
#define TRAINER_PANEL_ROW_H   48
#define TRAINER_PANEL_LINE_H  14
#define TRAINER_PANEL_TEXT_X  36  // Right of the icon
#define TRAINER_PANEL_MOVE_W  102 // Fits the longest move names

enum
{
    PANEL_NONE,
    PANEL_WILD,
    PANEL_TRAINER,
};

#define MAX_VIEWER_OBJECTS      64 // OBJECT_EVENT_TEMPLATES_COUNT
#define MAX_TRAINERS_PER_OBJECT 8  // e.g. the Champion has one team per starter, plus rematches
#define MAX_PANEL_ICONS         WILD_PANEL_MAX_SLOTS // More than PARTY_SIZE

// Kept for as long as the viewer is open
struct MapViewerExtra
{
    u8 panel; // PANEL_*
    u8 numIcons;
    u8 iconSpriteIds[MAX_PANEL_ICONS];
    u8 trainerObject; // Object whose party is shown
    u8 trainerPage;   // Which of that object's trainer IDs is shown
    u8 trainerPartyPage; // Which TRAINER_PANEL_ROWS of the party are shown
    // Trainer IDs each object can battle with, found from the scripts when the map is loaded
    u8 numObjectTrainers[MAX_VIEWER_OBJECTS];
    u16 objectTrainers[MAX_VIEWER_OBJECTS][MAX_TRAINERS_PER_OBJECT];
    // While a panel is up, its icons may use any sprite palette slot (the map's sprites are hidden),
    // so the map's sprite palettes are put aside and restored when it closes
    u8 savedReservedPalCount;
    u16 savedObjPalsUnfaded[OBJ_PLTT_SIZE / sizeof(u16)];
    u16 savedObjPalsFaded[OBJ_PLTT_SIZE / sizeof(u16)];
};

// Cursor: 16x16 square outline, black outside and white inside so it shows up on anything
#define TAG_CURSOR 0x4D56

static const u32 ALIGNED(4) sCursorGfx[] =
{
    // Top left
    0x22222222, 0x11111112, 0x00000012, 0x00000012, 0x00000012, 0x00000012, 0x00000012, 0x00000012,
    // Top right
    0x22222222, 0x21111111, 0x21000000, 0x21000000, 0x21000000, 0x21000000, 0x21000000, 0x21000000,
    // Bottom left
    0x00000012, 0x00000012, 0x00000012, 0x00000012, 0x00000012, 0x00000012, 0x11111112, 0x22222222,
    // Bottom right
    0x21000000, 0x21000000, 0x21000000, 0x21000000, 0x21000000, 0x21000000, 0x21111111, 0x22222222,
};

static const u16 ALIGNED(4) sCursorPal[16] = {RGB_BLACK, RGB_WHITE, RGB_BLACK};

static const struct SpriteSheet ALIGNED(4) sCursorSpriteSheet = {sCursorGfx, sizeof(sCursorGfx), TAG_CURSOR};
static const struct SpritePalette ALIGNED(4) sCursorSpritePalette = {sCursorPal, TAG_CURSOR};

static const struct OamData ALIGNED(4) sCursorOamData =
{
    .shape = SPRITE_SHAPE(16x16),
    .size = SPRITE_SIZE(16x16),
    .priority = 0, // In front of all object sprites
};

static const struct SpriteTemplate ALIGNED(4) sCursorSpriteTemplate =
{
    .tileTag = TAG_CURSOR,
    .paletteTag = TAG_CURSOR,
    .oam = &sCursorOamData,
    .anims = gDummySpriteAnimTable,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCallbackDummy,
};

// Task data
#define tState       data[0]
#define tScrollX     data[1]
#define tScrollY     data[2]
#define tHeader      3 // Word arg, data[3] and data[4]
#define tCallback    5 // Word arg, data[5] and data[6]
#define tCursorX     data[7]
#define tCursorY     data[8]
#define tCursorSprite data[9]
#define tInfoWindow  data[10]
#define tExtra       11 // Word arg, data[11] and data[12]: struct MapViewerExtra
#define tMapGroup    data[13]
#define tMapNum      data[14]
#define tWildMethod  data[15]

// Sprite data, for both object sprites and the cursor
#define sMapX         data[0]
#define sMapY         data[1]
#define sElevation    data[2]
#define sOnMap        data[7] // Set to MAP_SPRITE_TAG, so the wild panel's icons aren't moved with the map
#define MAP_SPRITE_TAG 0x4D56

extern u8 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *mapLayout, u16 metatile, u8 attributeType);
extern const u8 gInitialMovementTypeFacingDirections[];
extern const u8 sElevationToPriority[];
extern const u8 sElevationToSubspriteTableNum[];
extern u8 FindOrLoadNPCPalette(u16 palTag); // CFRU's dynamic overworld palettes (dynamic_ow_pals.c), already in the ROM

extern const u8 gText_MapViewer_WarpInfo[];
extern const u8 gText_MapViewer_DynamicWarp[];
extern const u8 gText_MapViewer_NoWildMons[];
extern const u8 gText_MapViewer_ChangePage[];
extern const u8 gText_MapViewer_Lv[];
extern const u8 gText_MapViewer_DpadLeftRight[];
extern const u8 gText_MapViewer_DpadUpDown[];
extern const u8 gText_MapViewer_Double[];
extern const u8 gText_MapViewer_IV[];
extern const u8 gText_MapViewer_NoItem[];
extern const u8 gText_MapViewer_NoMove[];

static void Task_WaitFadeThenOpenCurrentMapViewer(u8 taskId);
static void CB2_OpenCurrentMapViewer(void);
static void CB2_MapViewer(void);
static void VBlankCB_MapViewer(void);
static void Task_MapViewerInit(u8 taskId);
static void Task_MapViewerLoadMap(u8 taskId);
static void Task_MapViewerWaitFadeThenLoadMap(u8 taskId);
static void Task_MapViewer(u8 taskId);
static void Task_MapViewerExit(u8 taskId);
static void MapViewer_Redraw(const struct MapHeader *header, s16 scrollX, s16 scrollY);
static void MapViewer_RefreshOverlay(u8 taskId);
static bool8 MapViewer_ShouldShowObject(const struct ObjectEventTemplate *template);
static void MapViewer_CreateObjectSprites(const struct MapHeader *header);
static void MapViewer_UpdateSprites(s16 scrollX, s16 scrollY);
static bool8 MapViewer_HandlePanels(u8 taskId);
static void MapViewer_FindObjectTrainers(u8 taskId);
static bool8 MapViewer_IsTrainerObject(const struct MapViewerExtra *extra, const struct ObjectEventTemplate *objects, u8 i);

void ShowCurrentMapViewer(void)
{
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    CreateTask(Task_WaitFadeThenOpenCurrentMapViewer, 0);
}

static void Task_WaitFadeThenOpenCurrentMapViewer(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        CleanupOverworldWindowsAndTilemaps();
        DestroyTask(taskId);
        SetMainCallback2(CB2_OpenCurrentMapViewer);
    }
}

static void CB2_OpenCurrentMapViewer(void)
{
    OpenMapViewer(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum, CB2_ReturnToFieldContinueScript);
}

void OpenMapViewer(u8 mapGroup, u8 mapNum, MainCallback returnCallback)
{
    u8 taskId;
    const struct MapHeader *header = Overworld_GetMapHeaderByGroupAndId(mapGroup, mapNum);

    if (header == NULL || header->mapLayout == NULL)
    {
        SetMainCallback2(returnCallback);
        return;
    }

    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    ScanlineEffect_Stop();
    ResetSpriteData();
    ResetPaletteFade();
    FreeAllSpritePalettes();
    ResetTasks();

    taskId = CreateTask(Task_MapViewerInit, 0);
    SetWordTaskArg(taskId, tHeader, (u32)header);
    SetWordTaskArg(taskId, tCallback, (u32)returnCallback);
    SetWordTaskArg(taskId, tExtra, (u32)AllocZeroed(sizeof(struct MapViewerExtra))); // Freed in Task_MapViewerExit
    gTasks[taskId].tMapGroup = mapGroup;
    gTasks[taskId].tMapNum = mapNum;
    gTasks[taskId].tWildMethod = WILD_METHOD_LAND;
    SetMainCallback2(CB2_MapViewer);
}

static void CB2_MapViewer(void)
{
    RunTasks();
    RunTextPrinters();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
    DoScheduledBgTilemapCopiesToVram();
}

static void VBlankCB_MapViewer(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

// Puts the cursor on a metatile, with the view centred on it as far as the map allows
static void MapViewer_SetCursor(u8 taskId, s16 x, s16 y)
{
    s16 *data = gTasks[taskId].data;
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    s16 maxX = max(0, header->mapLayout->width - MAP_VIEWER_WIDTH);
    s16 maxY = max(0, header->mapLayout->height - MAP_VIEWER_HEIGHT);

    tCursorX = x;
    tCursorY = y;
    tScrollX = min(max(0, x - MAP_VIEWER_WIDTH / 2), maxX);
    tScrollY = min(max(0, y - MAP_VIEWER_HEIGHT / 2), maxY);
}

// One-time setup of everything that doesn't depend on the map, then loads the first map
static void Task_MapViewerInit(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    DmaFill16(3, 0, (void *)VRAM, VRAM_SIZE);
    DmaFill32(3, 0, (void *)OAM, OAM_SIZE);
    DmaFill16(3, 0, (void *)PLTT, PLTT_SIZE);
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sMapViewerBgTemplates, NELEMS(sMapViewerBgTemplates));
    SetBgTilemapBuffer(BG_TOP, AllocZeroed(MAP_VIEWER_TILEMAP_SIZE));
    SetBgTilemapBuffer(BG_MIDDLE, AllocZeroed(MAP_VIEWER_TILEMAP_SIZE));
    SetBgTilemapBuffer(BG_BOTTOM, AllocZeroed(MAP_VIEWER_TILEMAP_SIZE));
    SetBgTilemapBuffer(BG_OVERLAY, AllocZeroed(MAP_VIEWER_TILEMAP_SIZE)); // Before InitWindows, so it uses this buffer
    ChangeBgX(BG_OVERLAY, 0, BG_COORD_SET);
    ChangeBgY(BG_OVERLAY, 0, BG_COORD_SET);
    ChangeBgX(BG_TOP, 0, BG_COORD_SET);
    ChangeBgY(BG_TOP, 0, BG_COORD_SET);
    ChangeBgX(BG_MIDDLE, 0, BG_COORD_SET);
    ChangeBgY(BG_MIDDLE, 0, BG_COORD_SET);
    ChangeBgX(BG_BOTTOM, 0, BG_COORD_SET);
    ChangeBgY(BG_BOTTOM, 0, BG_COORD_SET);
    InitWindows(sMapViewerWindowTemplates);
    DeactivateAllTextPrinters();

    LoadBgTiles(BG_OVERLAY, sHighlightTile, sizeof(sHighlightTile), HIGHLIGHT_TILE);
    LoadPalette(&sHighlightWarpColor, BG_PLTT_ID(PALSLOT_HIGHLIGHT_WARP) + 1, sizeof(u16));
    LoadPalette(&sHighlightTrainerColor, BG_PLTT_ID(PALSLOT_HIGHLIGHT_TRAINER) + 1, sizeof(u16));
    LoadPalette(sInfoTextPal, BG_PLTT_ID(PALSLOT_INFO_TEXT), sizeof(sInfoTextPal));
    BlendPalettes(PALETTES_ALL, 16, RGB_BLACK); // Stays black until the map is loaded and faded in

    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
    // Highlights are blended half and half with the map and sprites under them
    SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_BLEND
                               | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ);
    SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(8, 8));
    // Window 0 covers the info box while it's shown: no blending and no sprites there, so the box is solid
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, DISPLAY_WIDTH));
    SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL);
    SetGpuReg(REG_OFFSET_WINOUT, WINOUT_WIN01_ALL);
    ShowBg(BG_OVERLAY);
    ShowBg(BG_TOP);
    ShowBg(BG_MIDDLE);
    ShowBg(BG_BOTTOM);
    SetVBlankCallback(VBlankCB_MapViewer);

    tInfoWindow = WIN_INFO_NONE;
    MapViewer_SetCursor(taskId, 0, 0); // Start at the top left corner
    tState = 0;
    gTasks[taskId].func = Task_MapViewerLoadMap;
}

// Loads tHeader's map with the cursor where it was set. The screen must already be black.
// Each step that loads palettes blends them back to black, since the VBlank callback is running.
static void Task_MapViewerLoadMap(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);

    switch (tState)
    {
    case 0:
        CopyMapTilesetsToVram(header->mapLayout);
        LoadMapTilesetPalettes(header->mapLayout);
        BlendPalettes(PALETTES_ALL, 16, RGB_BLACK);
        tState++;
        break;
    case 1:
        MapViewer_Redraw(header, tScrollX, tScrollY);

        // Rebuild every sprite for the new map
        ResetSpriteData();
        FreeAllSpritePalettes();
        MapViewer_CreateObjectSprites(header);
        LoadSpriteSheet(&sCursorSpriteSheet);
        LoadSpritePalette(&sCursorSpritePalette);
        tCursorSprite = CreateSprite(&sCursorSpriteTemplate, 0, 0, 0);
        gSprites[tCursorSprite].sOnMap = MAP_SPRITE_TAG;
        MapViewer_FindObjectTrainers(taskId);
        MapViewer_RefreshOverlay(taskId); // Also positions the cursor
        MapViewer_UpdateSprites(tScrollX, tScrollY);
        BlendPalettes(PALETTES_ALL, 16, RGB_BLACK);
        tState++;
        break;
    case 2:
        if (IsDma3ManagerBusyWithBgCopy())
            break;
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        gTasks[taskId].func = Task_MapViewer;
        break;
    }
}

static void Task_MapViewerWaitFadeThenLoadMap(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        gTasks[taskId].tState = 0;
        gTasks[taskId].func = Task_MapViewerLoadMap;
    }
}

// The map a warp leads to, or NULL if it can't be shown (dynamic warps lead wherever the player came from)
static const struct MapHeader *MapViewer_GetWarpDestination(const struct WarpEvent *warp)
{
    const struct MapHeader *destHeader;

    if (warp == NULL)
        return NULL;
    if (warp->mapGroup == MAP_DYNAMIC_GROUP && warp->mapNum == MAP_DYNAMIC_NUM)
        return NULL;

    destHeader = Overworld_GetMapHeaderByGroupAndId(warp->mapGroup, warp->mapNum);
    if (destHeader == NULL || destHeader->mapLayout == NULL)
        return NULL;
    return destHeader;
}

// Switches the viewer to the warp's destination map, with the cursor on the target warp
static bool8 MapViewer_TryFollowWarp(u8 taskId, const struct WarpEvent *warp)
{
    const struct MapHeader *destHeader = MapViewer_GetWarpDestination(warp);
    const struct MapEvents *destEvents;
    s16 x = 0, y = 0;

    if (destHeader == NULL)
        return FALSE;

    destEvents = destHeader->events;
    if (destEvents != NULL && warp->warpId < destEvents->warpCount)
    {
        x = destEvents->warps[warp->warpId].x;
        y = destEvents->warps[warp->warpId].y;
    }

    SetWordTaskArg(taskId, tHeader, (u32)destHeader);
    gTasks[taskId].tMapGroup = warp->mapGroup;
    gTasks[taskId].tMapNum = warp->mapNum;
    MapViewer_SetCursor(taskId, x, y);
    PlaySE(SE_WARP_IN);
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    gTasks[taskId].func = Task_MapViewerWaitFadeThenLoadMap;
    return TRUE;
}

static void DrawMetatileLayer(u8 bg, u16 offset, const u16 *tiles)
{
    u16 *tilemap = GetBgTilemapBuffer(bg);

    tilemap[offset] = tiles[0];
    tilemap[offset + 1] = tiles[1];
    tilemap[offset + 32] = tiles[2];
    tilemap[offset + 33] = tiles[3];
}

// The layer types as the ROM's DrawMetatile handles them, with CFRU's triple layer blocks (DrawBlockHook at
// 0x805A9B8). Its jump table (0x8880330) sends 1 and 2 to the opposite cases to pokefirered's
// METATILE_LAYER_TYPE_COVERED and _SPLIT, so they're named here by what they draw.
#define LAYER_TYPE_MIDDLE_TOP    0 // Normal
#define LAYER_TYPE_BOTTOM_TOP    1
#define LAYER_TYPE_BOTTOM_MIDDLE 2
#define LAYER_TYPE_TRIPLE        3 // Uses 12 tiles: the third layer is the first 4 tiles of the next metatile

static void FillMetatileLayer(u8 bg, u16 offset, u16 tile)
{
    u16 *tilemap = GetBgTilemapBuffer(bg);

    tilemap[offset] = tile;
    tilemap[offset + 1] = tile;
    tilemap[offset + 32] = tile;
    tilemap[offset + 33] = tile;
}

// Mirrors DrawMetatile in field_camera.c as patched by DrawBlockHook
static void DrawMetatile(u8 layerType, const u16 *tiles, u16 offset)
{
    switch (layerType)
    {
    case LAYER_TYPE_MIDDLE_TOP:
        FillMetatileLayer(BG_BOTTOM, offset, 0x3014); // The same filler tile the game puts under these
        DrawMetatileLayer(BG_MIDDLE, offset, tiles);
        DrawMetatileLayer(BG_TOP, offset, tiles + 4);
        break;
    case LAYER_TYPE_BOTTOM_TOP:
        DrawMetatileLayer(BG_BOTTOM, offset, tiles);
        DrawMetatileLayer(BG_TOP, offset, tiles + 4);
        break;
    case LAYER_TYPE_BOTTOM_MIDDLE:
        DrawMetatileLayer(BG_BOTTOM, offset, tiles);
        DrawMetatileLayer(BG_MIDDLE, offset, tiles + 4);
        break;
    case LAYER_TYPE_TRIPLE:
        DrawMetatileLayer(BG_BOTTOM, offset, tiles);
        DrawMetatileLayer(BG_MIDDLE, offset, tiles + 4);
        DrawMetatileLayer(BG_TOP, offset, tiles + 8);
        break;
    default: // The game draws nothing for these either
        break;
    }
}

static void MapViewer_Redraw(const struct MapHeader *header, s16 scrollX, s16 scrollY)
{
    s16 x, y;
    const struct MapLayout *layout = header->mapLayout;

    CpuFill16(0, GetBgTilemapBuffer(BG_TOP), MAP_VIEWER_TILEMAP_SIZE);
    CpuFill16(0, GetBgTilemapBuffer(BG_MIDDLE), MAP_VIEWER_TILEMAP_SIZE);
    CpuFill16(0, GetBgTilemapBuffer(BG_BOTTOM), MAP_VIEWER_TILEMAP_SIZE);

    for (y = 0; y < MAP_VIEWER_HEIGHT; y++)
    {
        for (x = 0; x < MAP_VIEWER_WIDTH; x++)
        {
            s16 mapX = scrollX + x;
            s16 mapY = scrollY + y;
            u16 metatile;
            const u16 *tiles;

            if (mapX >= layout->width || mapY >= layout->height)
                continue; // Leave blank

            metatile = layout->map[mapY * layout->width + mapX] & MAPGRID_METATILE_ID_MASK;
            if (metatile < NUM_METATILES_IN_PRIMARY)
                tiles = layout->primaryTileset->metatiles + metatile * NUM_TILES_PER_METATILE;
            else
                tiles = layout->secondaryTileset->metatiles + (metatile - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;

            DrawMetatile(GetAttributeByMetatileIdAndMapLayout(layout, metatile, METATILE_ATTRIBUTE_LAYER_TYPE),
                         tiles, y * 2 * 32 + x * 2);
        }
    }

    ScheduleBgCopyTilemapToVram(BG_TOP);
    ScheduleBgCopyTilemapToVram(BG_MIDDLE);
    ScheduleBgCopyTilemapToVram(BG_BOTTOM);
}

static void MapViewer_DrawHighlight(s16 mapX, s16 mapY, s16 scrollX, s16 scrollY, u8 palSlot)
{
    u16 *tilemap = GetBgTilemapBuffer(BG_OVERLAY);
    u16 tile = HIGHLIGHT_TILE | (palSlot << 12);
    s16 x = mapX - scrollX;
    s16 y = mapY - scrollY;
    u16 offset;

    if (x < 0 || y < 0 || x >= MAP_VIEWER_WIDTH || y >= MAP_VIEWER_HEIGHT)
        return;

    offset = y * 2 * 32 + x * 2;
    tilemap[offset] = tile;
    tilemap[offset + 1] = tile;
    tilemap[offset + 32] = tile;
    tilemap[offset + 33] = tile;
}

// Warps in yellow, trainers in red
static void MapViewer_DrawHighlights(const struct MapHeader *header, const struct MapViewerExtra *extra, s16 scrollX, s16 scrollY)
{
    u8 i;
    const struct MapEvents *events = header->events;

    CpuFill16(0, GetBgTilemapBuffer(BG_OVERLAY), MAP_VIEWER_TILEMAP_SIZE);

    if (events == NULL)
        return;

    for (i = 0; i < events->warpCount; i++)
        MapViewer_DrawHighlight(events->warps[i].x, events->warps[i].y, scrollX, scrollY, PALSLOT_HIGHLIGHT_WARP);

    for (i = 0; i < events->objectEventCount; i++)
    {
        const struct ObjectEventTemplate *template = &events->objectEvents[i];

        if (MapViewer_ShouldShowObject(template) && MapViewer_IsTrainerObject(extra, events->objectEvents, i))
            MapViewer_DrawHighlight(template->x, template->y, scrollX, scrollY, PALSLOT_HIGHLIGHT_TRAINER);
    }
}

static const struct WarpEvent *MapViewer_GetWarpAt(const struct MapHeader *header, s16 x, s16 y)
{
    u8 i;
    const struct MapEvents *events = header->events;

    if (events == NULL)
        return NULL;

    for (i = 0; i < events->warpCount; i++)
    {
        if (events->warps[i].x == x && events->warps[i].y == y)
            return &events->warps[i];
    }
    return NULL;
}

static void MapViewer_PrintWarpInfo(u8 windowId, const struct WarpEvent *warp)
{
    if (warp->mapGroup == MAP_DYNAMIC_GROUP && warp->mapNum == MAP_DYNAMIC_NUM)
    {
        StringCopy(gStringVar4, gText_MapViewer_DynamicWarp);
    }
    else
    {
        const struct MapHeader *destHeader = Overworld_GetMapHeaderByGroupAndId(warp->mapGroup, warp->mapNum);

        if (destHeader != NULL)
            GetMapName(gStringVar1, destHeader->regionMapSectionId, 0);
        else
            gStringVar1[0] = EOS;
        ConvertIntToDecimalStringN(gStringVar2, warp->mapGroup, STR_CONV_MODE_LEFT_ALIGN, 3);
        ConvertIntToDecimalStringN(gStringVar3, warp->mapNum, STR_CONV_MODE_LEFT_ALIGN, 3);
        StringExpandPlaceholders(gStringVar4, gText_MapViewer_WarpInfo);
    }

    FillWindowPixelBuffer(windowId, PIXEL_FILL(1));
    AddTextPrinterParameterized3(windowId, FONT_NORMAL, 4, 1, sInfoTextColors, 0, gStringVar4);
    PutWindowTilemap(windowId);
    CopyWindowToVram(windowId, COPYWIN_GFX);
}

// Redraws the highlights, and shows the warp info box if the cursor is on a warp
static void MapViewer_RefreshOverlay(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    const struct WarpEvent *warp = MapViewer_GetWarpAt(header, tCursorX, tCursorY);

    MapViewer_DrawHighlights(header, (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra), tScrollX, tScrollY);

    if (warp == NULL)
    {
        tInfoWindow = WIN_INFO_NONE;
        ClearGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN0_ON);
    }
    else
    {
        if (tCursorY - tScrollY >= MAP_VIEWER_HEIGHT / 2)
        {
            tInfoWindow = WIN_INFO_TOP;
            SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(0, INFO_WINDOW_HEIGHT * 8));
        }
        else
        {
            tInfoWindow = WIN_INFO_BOTTOM;
            SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(DISPLAY_HEIGHT - INFO_WINDOW_HEIGHT * 8, DISPLAY_HEIGHT));
        }
        MapViewer_PrintWarpInfo(tInfoWindow, warp);
        SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, DISPLAY_WIDTH));
        SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG_ALL); // No sprites over the box
        SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN0_ON);
    }

    ScheduleBgCopyTilemapToVram(BG_OVERLAY);

    gSprites[tCursorSprite].sMapX = tCursorX;
    gSprites[tCursorSprite].sMapY = tCursorY;
}

// Same checks as TrySpawnObjectEventTemplate, plus objects that never have a visible sprite
static bool8 MapViewer_ShouldShowObject(const struct ObjectEventTemplate *template)
{
    if (template->kind == OBJ_KIND_CLONE) // Copies of objects from connected maps
        return FALSE;
    if (FlagGet(template->flagId)) // Hidden, e.g. item balls that were already picked up
        return FALSE;
    if (template->objUnion.normal.movementType == MOVEMENT_TYPE_INVISIBLE)
        return FALSE;
    return TRUE;
}

// Mirrors TrySetupObjectEventSprite in event_object_movement.c, with CFRU's dynamic overworld palettes.
// Returns the palette slot it used, or 0xFF if it couldn't make the sprite.
static u8 MapViewer_CreateObjectSprite(const struct ObjectEventTemplate *template)
{
    u8 spriteId;
    u8 graphicsId = template->graphicsId;
    u8 elevation = template->objUnion.normal.elevation;
    struct Sprite *sprite;
    struct SpriteTemplate spriteTemplate;
    const struct SubspriteTable *subspriteTables;
    const struct ObjectEventGraphicsInfo *graphicsInfo;

    if (graphicsId >= OBJ_EVENT_GFX_VARS)
        graphicsId = VarGetObjectEventGraphicsId(graphicsId - OBJ_EVENT_GFX_VARS);

    graphicsInfo = GetObjectEventGraphicsInfo(graphicsId);

    CopyObjectGraphicsInfoToSpriteTemplate(graphicsId, SpriteCallbackDummy, &spriteTemplate, &subspriteTables);
    spriteTemplate.paletteTag = TAG_NONE;
    spriteId = CreateSprite(&spriteTemplate, 0, 0, 0);
    if (spriteId == MAX_SPRITES)
        return 0xFF;

    sprite = &gSprites[spriteId];
    sprite->centerToCornerVecX = -(graphicsInfo->width >> 1);
    sprite->centerToCornerVecY = -(graphicsInfo->height >> 1);
    // Same as CFRU's SetPalNPC hooks: the palette is loaded into whichever slot is free (or already has it),
    // and DestroySprite releases it again
    sprite->oam.paletteNum = FindOrLoadNPCPalette(graphicsInfo->paletteTag);
    if (subspriteTables != NULL)
    {
        SetSubspriteTables(sprite, subspriteTables);
        sprite->subspriteMode = SUBSPRITES_IGNORE_PRIORITY;
    }
    sprite->subspriteTableNum = sElevationToSubspriteTableNum[elevation];
    sprite->oam.priority = sElevationToPriority[elevation];
    if (!graphicsInfo->inanimate)
        StartSpriteAnim(sprite, GetFaceDirectionAnimNum(gInitialMovementTypeFacingDirections[template->objUnion.normal.movementType]));

    sprite->sMapX = template->x;
    sprite->sMapY = template->y;
    sprite->sElevation = elevation;
    sprite->sOnMap = MAP_SPRITE_TAG;
    return sprite->oam.paletteNum;
}

// Must come straight after ResetSpriteData and FreeAllSpritePalettes. CFRU hooks ResetSpriteData to clear its
// overworld palette references, so the NPC palettes are handed out from slot 0 up.
static void MapViewer_CreateObjectSprites(const struct MapHeader *header)
{
    u8 i, palSlot;
    u8 numNpcPalSlots = 0;
    const struct MapEvents *events = header->events;

    if (events != NULL)
    {
        for (i = 0; i < events->objectEventCount; i++)
        {
            if (!MapViewer_ShouldShowObject(&events->objectEvents[i]))
                continue;

            palSlot = MapViewer_CreateObjectSprite(&events->objectEvents[i]);
            if (palSlot != 0xFF && palSlot >= numNpcPalSlots)
                numNpcPalSlots = palSlot + 1;
        }
    }

    // Other sprite palettes (the cursor and the panels' Pokemon icons) go in the slots after the NPCs'
    gReservedSpritePaletteCount = numNpcPalSlots;
}

// Every sprite in the viewer (objects and the cursor) sits on a metatile, so just move them all
static void MapViewer_UpdateSprites(s16 scrollX, s16 scrollY)
{
    u8 i;

    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];

        if (!sprite->inUse || sprite->sOnMap != MAP_SPRITE_TAG)
            continue;

        // Bottom of the sprite sits on the bottom of its metatile, as in the field
        sprite->x = (sprite->sMapX - scrollX) * 16 + 8;
        sprite->y = (sprite->sMapY - scrollY) * 16 + 16 + sprite->centerToCornerVecY;
        // OAM coordinates wrap, so anything well off screen must be hidden
        sprite->invisible = (sprite->x < -32 || sprite->x > DISPLAY_WIDTH + 32
                          || sprite->y < -32 || sprite->y > DISPLAY_HEIGHT + 32);
        SetObjectSubpriorityByElevation(sprite->sElevation, sprite, 1);
    }
}

static const struct WildPokemonHeader *MapViewer_GetWildHeader(u8 mapGroup, u8 mapNum)
{
    u16 i;

    for (i = 0; gWildMonHeaders[i].mapGroup != MAP_UNDEFINED_GROUP; i++)
    {
        if (gWildMonHeaders[i].mapGroup == mapGroup && gWildMonHeaders[i].mapNum == mapNum)
            return &gWildMonHeaders[i];
    }
    return NULL;
}

static const struct WildPokemonInfo *MapViewer_GetWildInfo(const struct WildPokemonHeader *wild, u8 method)
{
    if (wild == NULL)
        return NULL;

    switch (method)
    {
    case WILD_METHOD_LAND:
        return wild->landMonsInfo;
    case WILD_METHOD_WATER:
        return wild->waterMonsInfo;
    case WILD_METHOD_ROCK_SMASH:
        return wild->rockSmashMonsInfo;
    default: // The rods
        return wild->fishingMonsInfo;
    }
}

// Moves tWildMethod to the nearest method this map has, in the given direction.
// A direction of 0 keeps the current method if the map has it.
static bool8 MapViewer_SelectWildMethod(u8 taskId, s8 direction)
{
    s16 *data = gTasks[taskId].data;
    const struct WildPokemonHeader *wild = MapViewer_GetWildHeader(tMapGroup, tMapNum);
    s8 step = (direction < 0) ? -1 : 1;
    s16 method = tWildMethod;
    u8 i;

    if (direction != 0)
        method += step;

    for (i = 0; i < WILD_METHOD_COUNT; i++, method += step)
    {
        method = (method + WILD_METHOD_COUNT) % WILD_METHOD_COUNT;
        if (MapViewer_GetWildInfo(wild, method) != NULL)
        {
            tWildMethod = method;
            return TRUE;
        }
    }
    return FALSE;
}

static u8 MapViewer_CountWildMethods(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct WildPokemonHeader *wild = MapViewer_GetWildHeader(tMapGroup, tMapNum);
    u8 i, count = 0;

    for (i = 0; i < WILD_METHOD_COUNT; i++)
    {
        if (MapViewer_GetWildInfo(wild, i) != NULL)
            count++;
    }
    return count;
}

// --- Finding trainers from scripts ---

// Length in bytes of each FireRed script command, from the macros in asm/macros/event.inc
static const u8 sScriptCmdLengths[] =
{
    [0x00] =  1, // nop
    [0x01] =  1, // nop1
    [0x02] =  1, // end
    [0x03] =  1, // return
    [0x04] =  5, // call
    [0x05] =  5, // goto
    [0x06] =  6, // goto_if
    [0x07] =  6, // call_if
    [0x08] =  2, // gotostd
    [0x09] =  2, // callstd
    [0x0A] =  3, // gotostd_if
    [0x0B] =  3, // callstd_if
    [0x0C] =  1, // returnram
    [0x0D] =  1, // endram
    [0x0E] =  2, // setmysteryeventstatus
    [0x0F] =  6, // loadword
    [0x10] =  3, // loadbyte
    [0x11] =  6, // setptr
    [0x12] =  6, // loadbytefromptr
    [0x13] =  6, // setptrbyte
    [0x14] =  3, // copylocal
    [0x15] =  9, // copybyte
    [0x16] =  5, // setvar
    [0x17] =  5, // addvar
    [0x18] =  5, // subvar
    [0x19] =  5, // copyvar
    [0x1A] =  5, // setorcopyvar
    [0x1B] =  3, // compare_local_to_local
    [0x1C] =  3, // compare_local_to_value
    [0x1D] =  6, // compare_local_to_ptr
    [0x1E] =  6, // compare_ptr_to_local
    [0x1F] =  6, // compare_ptr_to_value
    [0x20] =  9, // compare_ptr_to_ptr
    [0x21] =  5, // compare_var_to_value
    [0x22] =  5, // compare_var_to_var
    [0x23] =  5, // callnative
    [0x24] =  5, // gotonative
    [0x25] =  3, // special
    [0x26] =  5, // specialvar
    [0x27] =  1, // waitstate
    [0x28] =  3, // delay
    [0x29] =  3, // setflag
    [0x2A] =  3, // clearflag
    [0x2B] =  3, // checkflag
    [0x2C] =  5, // initclock
    [0x2D] =  1, // dotimebasedevents
    [0x2E] =  1, // gettime
    [0x2F] =  3, // playse
    [0x30] =  1, // waitse
    [0x31] =  3, // playfanfare
    [0x32] =  1, // waitfanfare
    [0x33] =  4, // playbgm
    [0x34] =  3, // savebgm
    [0x35] =  1, // fadedefaultbgm
    [0x36] =  3, // fadenewbgm
    [0x37] =  2, // fadeoutbgm
    [0x38] =  2, // fadeinbgm
    [0x39] =  8, // warp
    [0x3A] =  8, // warpsilent
    [0x3B] =  8, // warpdoor
    [0x3C] =  3, // warphole
    [0x3D] =  8, // warpteleport
    [0x3E] =  8, // setwarp
    [0x3F] =  8, // setdynamicwarp
    [0x40] =  8, // setdivewarp
    [0x41] =  8, // setholewarp
    [0x42] =  5, // getplayerxy
    [0x43] =  1, // getpartysize
    [0x44] =  5, // additem
    [0x45] =  5, // removeitem
    [0x46] =  5, // checkitemspace
    [0x47] =  5, // checkitem
    [0x48] =  3, // checkitemtype
    [0x49] =  5, // addpcitem
    [0x4A] =  5, // checkpcitem
    [0x4B] =  3, // adddecoration
    [0x4C] =  3, // removedecoration
    [0x4D] =  3, // checkdecor
    [0x4E] =  3, // checkdecorspace
    [0x4F] =  7, // applymovement
    [0x50] =  9, // applymovementat
    [0x51] =  3, // waitmovement
    [0x52] =  5, // waitmovementat
    [0x53] =  3, // removeobject
    [0x54] =  5, // removeobjectat
    [0x55] =  3, // addobject
    [0x56] =  5, // addobjectat
    [0x57] =  7, // setobjectxy
    [0x58] =  5, // showobjectat
    [0x59] =  5, // hideobjectat
    [0x5A] =  1, // faceplayer
    [0x5B] =  4, // turnobject
    [0x5C] =  0, // trainerbattle
    [0x5D] =  1, // dotrainerbattle
    [0x5E] =  1, // gotopostbattlescript
    [0x5F] =  1, // gotobeatenscript
    [0x60] =  3, // checktrainerflag
    [0x61] =  3, // settrainerflag
    [0x62] =  3, // cleartrainerflag
    [0x63] =  7, // setobjectxyperm
    [0x64] =  3, // copyobjectxytoperm
    [0x65] =  4, // setobjectmovementtype
    [0x66] =  1, // waitmessage
    [0x67] =  5, // message
    [0x68] =  1, // closemessage
    [0x69] =  1, // lockall
    [0x6A] =  1, // lock
    [0x6B] =  1, // releaseall
    [0x6C] =  1, // release
    [0x6D] =  1, // waitbuttonpress
    [0x6E] =  3, // yesnobox
    [0x6F] =  5, // multichoice
    [0x70] =  6, // multichoicedefault
    [0x71] =  6, // multichoicegrid
    [0x72] =  1, // drawbox
    [0x73] =  5, // erasebox
    [0x74] =  5, // drawboxtext
    [0x75] =  5, // showmonpic
    [0x76] =  1, // hidemonpic
    [0x77] =  2, // showcontestpainting
    [0x78] =  5, // braillemessage
    [0x79] = 15, // givemon
    [0x7A] =  3, // giveegg
    [0x7B] =  5, // setmonmove
    [0x7C] =  3, // checkpartymove
    [0x7D] =  4, // bufferspeciesname
    [0x7E] =  2, // bufferleadmonspeciesname
    [0x7F] =  4, // bufferpartymonnick
    [0x80] =  4, // bufferitemname
    [0x81] =  4, // bufferdecorationname
    [0x82] =  4, // buffermovename
    [0x83] =  4, // buffernumberstring
    [0x84] =  4, // bufferstdstring
    [0x85] =  6, // bufferstring
    [0x86] =  5, // pokemart
    [0x87] =  5, // pokemartdecoration
    [0x88] =  5, // pokemartdecoration2
    [0x89] =  3, // playslotmachine
    [0x8A] =  4, // setberrytree
    [0x8B] =  1, // choosecontestmon
    [0x8C] =  1, // startcontest
    [0x8D] =  1, // showcontestresults
    [0x8E] =  1, // contestlinktransfer
    [0x8F] =  3, // random
    [0x90] =  6, // addmoney
    [0x91] =  6, // removemoney
    [0x92] =  6, // checkmoney
    [0x93] =  4, // showmoneybox
    [0x94] =  3, // hidemoneybox
    [0x95] =  4, // updatemoneybox
    [0x96] =  3, // getpokenewsactive
    [0x97] =  2, // fadescreen
    [0x98] =  3, // fadescreenspeed
    [0x99] =  3, // setflashlevel
    [0x9A] =  2, // animateflash
    [0x9B] =  5, // messageautoscroll
    [0x9C] =  3, // dofieldeffect
    [0x9D] =  4, // setfieldeffectargument
    [0x9E] =  3, // waitfieldeffect
    [0x9F] =  3, // setrespawn
    [0xA0] =  1, // checkplayergender
    [0xA1] =  5, // playmoncry
    [0xA2] =  9, // setmetatile
    [0xA3] =  1, // resetweather
    [0xA4] =  3, // setweather
    [0xA5] =  1, // doweather
    [0xA6] =  2, // setstepcallback
    [0xA7] =  3, // setmaplayoutindex
    [0xA8] =  6, // setobjectsubpriority
    [0xA9] =  5, // resetobjectsubpriority
    [0xAA] =  9, // createvobject
    [0xAB] =  3, // turnvobject
    [0xAC] =  5, // opendoor
    [0xAD] =  5, // closedoor
    [0xAE] =  1, // waitdooranim
    [0xAF] =  5, // setdooropen
    [0xB0] =  5, // setdoorclosed
    [0xB1] =  8, // addelevmenuitem
    [0xB2] =  1, // showelevmenu
    [0xB3] =  3, // checkcoins
    [0xB4] =  3, // addcoins
    [0xB5] =  3, // removecoins
    [0xB6] =  6, // setwildbattle
    [0xB7] =  1, // dowildbattle
    [0xB8] =  5, // setvaddress
    [0xB9] =  5, // vgoto
    [0xBA] =  5, // vcall
    [0xBB] =  6, // vgoto_if
    [0xBC] =  6, // vcall_if
    [0xBD] =  5, // vmessage
    [0xBE] =  5, // vbuffermessage
    [0xBF] =  6, // vbufferstring
    [0xC0] =  3, // showcoinsbox
    [0xC1] =  3, // hidecoinsbox
    [0xC2] =  3, // updatecoinsbox
    [0xC3] =  2, // incrementgamestat
    [0xC4] =  8, // setescapewarp
    [0xC5] =  1, // waitmoncry
    [0xC6] =  4, // bufferboxname
    [0xC7] =  2, // textcolor
    [0xC8] =  5, // loadhelp
    [0xC9] =  1, // unloadhelp
    [0xCA] =  1, // signmsg
    [0xCB] =  1, // normalmsg
    [0xCC] =  6, // comparestat
    [0xCD] =  3, // setmonmodernfatefulencounter
    [0xCE] =  3, // checkmonmodernfatefulencounter
    [0xCF] =  1, // trywondercardscript
    [0xD0] =  3, // setworldmapflag
    [0xD1] =  8, // warpspinenter
    [0xD2] =  4, // setmonmetlocation
    [0xD3] =  5, // getbraillestringwidth
    [0xD4] =  6, // bufferitemnameplural
    [0xD5] =  1, // nop
};

// Bytes after trainerbattle's type, trainer and local ID, for each TRAINER_BATTLE_* type
static const u8 sTrainerBattleArgsLengths[] =
{
    [TRAINER_BATTLE_SINGLE]                          = 8,
    [TRAINER_BATTLE_CONTINUE_SCRIPT_NO_MUSIC]        = 12,
    [TRAINER_BATTLE_CONTINUE_SCRIPT]                 = 12,
    [TRAINER_BATTLE_SINGLE_NO_INTRO_TEXT]            = 4,
    [TRAINER_BATTLE_DOUBLE]                          = 12,
    [TRAINER_BATTLE_REMATCH]                         = 8,
    [TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE]          = 16,
    [TRAINER_BATTLE_REMATCH_DOUBLE]                  = 12,
    [TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE_NO_MUSIC] = 16,
    [TRAINER_BATTLE_EARLY_RIVAL]                     = 8,
};

#define IS_ROM_PTR(ptr) ((u32)(ptr) >= 0x08000000 && (u32)(ptr) < 0x0A000000)
#define SCRIPT_MAX_BRANCHES 32
#define SCRIPT_MAX_STEPS    1024

static const u8 *ReadScriptPtr(const u8 *ptr)
{
    return (const u8 *)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
}

static void AddTrainerId(u16 *ids, u8 *count, u16 trainerId)
{
    u8 i;

    for (i = 0; i < *count; i++)
    {
        if (ids[i] == trainerId)
            return;
    }
    if (*count < MAX_TRAINERS_PER_OBJECT)
        ids[(*count)++] = trainerId;
}

static void AddScriptBranch(const u8 **branches, u8 *numBranches, const u8 *script)
{
    u8 i;

    if (!IS_ROM_PTR(script))
        return;
    for (i = 0; i < *numBranches; i++)
    {
        if (branches[i] == script)
            return; // Already walked, or about to be
    }
    if (*numBranches < SCRIPT_MAX_BRANCHES)
        branches[(*numBranches)++] = script;
}

// Walks a script and every script it can goto/call (whatever the conditions), collecting the trainer of every trainerbattle
static void MapViewer_FindScriptTrainers(const u8 *script, u16 *ids, u8 *count)
{
    const u8 *branches[SCRIPT_MAX_BRANCHES];
    u8 numBranches = 0;
    u8 nextBranch = 0;
    u16 steps = 0;

    AddScriptBranch(branches, &numBranches, script);

    while (nextBranch < numBranches)
    {
        const u8 *ptr = branches[nextBranch++];
        bool8 endOfBranch = FALSE;

        while (!endOfBranch && steps++ < SCRIPT_MAX_STEPS)
        {
            u8 cmd = ptr[0];
            u8 length;

            if (cmd >= NELEMS(sScriptCmdLengths))
                break; // Unknown command (e.g. one a hack added), so its length isn't known

            length = sScriptCmdLengths[cmd];
            switch (cmd)
            {
            case 0x02: // end
            case 0x03: // return
            case 0x0C: // returnram
            case 0x0D: // endram
                endOfBranch = TRUE;
                break;
            case 0x05: // goto
                AddScriptBranch(branches, &numBranches, ReadScriptPtr(ptr + 1));
                endOfBranch = TRUE;
                break;
            case 0x04: // call
                AddScriptBranch(branches, &numBranches, ReadScriptPtr(ptr + 1));
                break;
            case 0x06: // goto_if
            case 0x07: // call_if
                AddScriptBranch(branches, &numBranches, ReadScriptPtr(ptr + 2));
                break;
            case 0x5C: // trainerbattle
            {
                u8 type = ptr[1];

                if (type >= NELEMS(sTrainerBattleArgsLengths))
                {
                    endOfBranch = TRUE;
                    break;
                }
                AddTrainerId(ids, count, ptr[2] | (ptr[3] << 8));
                length = 6 + sTrainerBattleArgsLengths[type];
                // These types carry on into another script after the battle
                if (type == TRAINER_BATTLE_CONTINUE_SCRIPT_NO_MUSIC || type == TRAINER_BATTLE_CONTINUE_SCRIPT
                 || type == TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE || type == TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE_NO_MUSIC)
                    AddScriptBranch(branches, &numBranches, ReadScriptPtr(ptr + length - 4));
                break;
            }
            }
            ptr += length;
        }
    }
}

// Battles started by the map itself (map scripts and coord events), like the Champion's
static void MapViewer_FindMapScriptTrainers(const struct MapHeader *header, u16 *ids, u8 *count)
{
    const u8 *mapScripts = header->mapScripts;
    const struct MapEvents *events = header->events;
    u8 i;

    if (IS_ROM_PTR(mapScripts))
    {
        // [u8 type][u32 script] entries, ending with type 0
        for (; mapScripts[0] != 0; mapScripts += 5)
        {
            const u8 *script = ReadScriptPtr(mapScripts + 1);

            if (!IS_ROM_PTR(script))
                continue;

            if (mapScripts[0] == MAP_SCRIPT_ON_FRAME_TABLE || mapScripts[0] == MAP_SCRIPT_ON_WARP_INTO_MAP_TABLE)
            {
                // [u16 var][u16 value][u32 script] entries, ending with var 0
                const u8 *entry;

                for (entry = script; (entry[0] | entry[1]) != 0; entry += 8)
                    MapViewer_FindScriptTrainers(ReadScriptPtr(entry + 4), ids, count);
            }
            else
            {
                MapViewer_FindScriptTrainers(script, ids, count);
            }
        }
    }

    if (events != NULL)
    {
        for (i = 0; i < events->coordEventCount; i++)
            MapViewer_FindScriptTrainers(events->coordEvents[i].script, ids, count);
    }
}

// Fills in which trainers each object can battle with. Gym Leaders and the Elite Four battle from their own
// scripts, but aren't marked as trainers in their object events. Objects with no script get any battles
// the map starts itself (e.g. the Champion, whose battle starts when the player walks in).
static void MapViewer_FindObjectTrainers(u8 taskId)
{
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    const struct MapEvents *events = header->events;
    u16 mapTrainers[MAX_TRAINERS_PER_OBJECT];
    u8 numMapTrainers = 0;
    u8 i;

    memset(extra->numObjectTrainers, 0, sizeof(extra->numObjectTrainers));
    if (events == NULL)
        return;

    MapViewer_FindMapScriptTrainers(header, mapTrainers, &numMapTrainers);

    for (i = 0; i < events->objectEventCount && i < MAX_VIEWER_OBJECTS; i++)
    {
        const struct ObjectEventTemplate *template = &events->objectEvents[i];

        if (template->kind == OBJ_KIND_CLONE)
            continue;

        if (IS_ROM_PTR(template->script))
        {
            MapViewer_FindScriptTrainers(template->script, extra->objectTrainers[i], &extra->numObjectTrainers[i]);
        }
        else if (numMapTrainers != 0)
        {
            memcpy(extra->objectTrainers[i], mapTrainers, numMapTrainers * sizeof(u16));
            extra->numObjectTrainers[i] = numMapTrainers;
        }
    }
}

static bool8 MapViewer_IsTrainerObject(const struct MapViewerExtra *extra, const struct ObjectEventTemplate *objects, u8 i)
{
    if (objects[i].objUnion.normal.trainerType != TRAINER_TYPE_NONE)
        return TRUE;
    return (i < MAX_VIEWER_OBJECTS && extra->numObjectTrainers[i] != 0);
}

// The shown trainer object under the cursor that has a known party, or 0xFF
static u8 MapViewer_GetTrainerObjectAtCursor(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapViewerExtra *extra = (const struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    const struct MapEvents *events = header->events;
    u8 i;

    if (events == NULL)
        return 0xFF;

    for (i = 0; i < events->objectEventCount && i < MAX_VIEWER_OBJECTS; i++)
    {
        const struct ObjectEventTemplate *template = &events->objectEvents[i];

        if (template->x == tCursorX && template->y == tCursorY
         && extra->numObjectTrainers[i] != 0 && MapViewer_ShouldShowObject(template))
            return i;
    }
    return 0xFF;
}

// --- Panels (hold L for wild encounters, R on a trainer for their party) ---

static void MapViewer_DestroyPanelIcons(struct MapViewerExtra *extra)
{
    u8 i;

    for (i = 0; i < extra->numIcons; i++)
        DestroyMonIcon(&gSprites[extra->iconSpriteIds[i]]);
    extra->numIcons = 0;
    FreeMonIconPalettes();
}

static void MapViewer_CreatePanelIcon(struct MapViewerExtra *extra, u16 species, s16 x, s16 y)
{
    u8 spriteId;

    LoadMonIconPalette(species);
    spriteId = CreateMonIcon(species, SpriteCB_MonIcon, x, y, 0, 0, FALSE);
    if (spriteId != MAX_SPRITES)
    {
        gSprites[spriteId].oam.priority = 0; // In front of the panel
        extra->iconSpriteIds[extra->numIcons++] = spriteId;
    }
}

// Prints one cell: the slot's icon, then its level range and chance to its right
static void MapViewer_DrawWildSlot(struct MapViewerExtra *extra, u8 cell, const struct WildPokemon *mon, u8 rate)
{
    u8 *str;
    u8 x = (cell % WILD_PANEL_COLUMNS) * WILD_PANEL_CELL_W;
    u8 y = WILD_PANEL_HEADER_H + (cell / WILD_PANEL_COLUMNS) * WILD_PANEL_CELL_H;

    str = StringCopy(gStringVar4, gText_MapViewer_Lv);
    str = ConvertIntToDecimalStringN(str, mon->minLevel, STR_CONV_MODE_LEFT_ALIGN, 3);
    if (mon->maxLevel != mon->minLevel)
    {
        *str++ = CHAR_HYPHEN;
        ConvertIntToDecimalStringN(str, mon->maxLevel, STR_CONV_MODE_LEFT_ALIGN, 3);
    }
    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, x + 36, y + 4, sPanelTextColors, 0, gStringVar4);

    str = ConvertIntToDecimalStringN(gStringVar4, rate, STR_CONV_MODE_LEFT_ALIGN, 3);
    *str++ = CHAR_PERCENT;
    *str = EOS;
    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, x + 36, y + 18, sPanelTextColors, 0, gStringVar4);

    MapViewer_CreatePanelIcon(extra, mon->species, x + 18, y + 18);
}

static void MapViewer_DrawWildPanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    const struct WildPokemonHeader *wild = MapViewer_GetWildHeader(tMapGroup, tMapNum);
    u8 i;

    MapViewer_DestroyPanelIcons(extra);
    FillWindowPixelBuffer(WIN_PANEL, PIXEL_FILL(1));

    if (!MapViewer_SelectWildMethod(taskId, 0))
    {
        AddTextPrinterParameterized3(WIN_PANEL, FONT_NORMAL, 4, 1, sPanelTextColors, 0, gText_MapViewer_NoWildMons);
    }
    else
    {
        const struct WildMethod *method = &sWildMethods[tWildMethod];
        const struct WildPokemonInfo *info = MapViewer_GetWildInfo(wild, tWildMethod);

        AddTextPrinterParameterized3(WIN_PANEL, FONT_NORMAL, 4, 1, sPanelTextColors, 0, method->name);
        if (MapViewer_CountWildMethods(taskId) > 1)
            AddTextPrinterParameterized3(WIN_PANEL, FONT_NORMAL,
                                         DISPLAY_WIDTH - 4 - GetStringWidth(FONT_NORMAL, gText_MapViewer_ChangePage, 0), 1,
                                         sPanelTextColors, 0, gText_MapViewer_ChangePage);

        // Every slot is shown separately, even when several have the same species
        for (i = 0; i < method->numSlots; i++)
            MapViewer_DrawWildSlot(extra, i, &info->wildPokemon[method->firstSlot + i], method->slotRates[i]);
    }

    PutWindowTilemap(WIN_PANEL);
    CopyWindowToVram(WIN_PANEL, COPYWIN_FULL);
}

// Same as GiveBoxMonInitialMoveset: the last 4 level up moves learned at or below the level
static void MapViewer_GetDefaultMoves(u16 species, u8 level, u16 *moves)
{
    const u16 *learnset = gLevelUpLearnsets[species];
    u8 i, j, numMoves = 0;

    for (i = 0; i < MAX_MON_MOVES; i++)
        moves[i] = MOVE_NONE;

    for (i = 0; learnset[i] != LEVEL_UP_END; i++)
    {
        u16 move = learnset[i] & LEVEL_UP_MOVE_ID;

        if ((learnset[i] & LEVEL_UP_MOVE_LV) > (level << 9))
            break;

        for (j = 0; j < numMoves; j++)
        {
            if (moves[j] == move)
                break;
        }
        if (j < numMoves)
            continue; // Already known

        if (numMoves < MAX_MON_MOVES)
        {
            moves[numMoves++] = move;
        }
        else
        {
            for (j = 0; j < MAX_MON_MOVES - 1; j++)
                moves[j] = moves[j + 1];
            moves[MAX_MON_MOVES - 1] = move;
        }
    }
}

// Trainer party members as the ROM lays them out. agbcc pads every struct to a multiple of 4 bytes, so these
// are 8 or 16 bytes long, where battle.h's versions (without the padding) are 6, 8, 14 and 16.
struct ALIGNED(4) RomTrainerMonNoItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
};

struct ALIGNED(4) RomTrainerMonNoItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 moves[MAX_MON_MOVES];
};

struct ALIGNED(4) RomTrainerMonItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
};

struct ALIGNED(4) RomTrainerMonItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
    u16 moves[MAX_MON_MOVES];
};

// Prints one row: icon, then species, level, IV and held item on the first line, and the moves two per line below
static void MapViewer_DrawTrainerMon(struct MapViewerExtra *extra, const struct Trainer *trainer, u8 slot, u8 row)
{
    u16 species, iv, heldItem = ITEM_NONE;
    u16 moves[MAX_MON_MOVES];
    u8 level, i;
    u8 *str;
    u8 y = WILD_PANEL_HEADER_H + row * TRAINER_PANEL_ROW_H;

    switch (trainer->partyFlags & (F_TRAINER_PARTY_CUSTOM_MOVESET | F_TRAINER_PARTY_HELD_ITEM))
    {
    case 0:
    {
        const struct RomTrainerMonNoItemDefaultMoves *mon = &((const struct RomTrainerMonNoItemDefaultMoves *)trainer->party.NoItemDefaultMoves)[slot];
        species = mon->species;
        level = mon->lvl;
        iv = mon->iv;
        MapViewer_GetDefaultMoves(species, level, moves);
        break;
    }
    case F_TRAINER_PARTY_CUSTOM_MOVESET:
    {
        const struct RomTrainerMonNoItemCustomMoves *mon = &((const struct RomTrainerMonNoItemCustomMoves *)trainer->party.NoItemCustomMoves)[slot];
        species = mon->species;
        level = mon->lvl;
        iv = mon->iv;
        memcpy(moves, mon->moves, sizeof(moves));
        break;
    }
    case F_TRAINER_PARTY_HELD_ITEM:
    {
        const struct RomTrainerMonItemDefaultMoves *mon = &((const struct RomTrainerMonItemDefaultMoves *)trainer->party.ItemDefaultMoves)[slot];
        species = mon->species;
        level = mon->lvl;
        iv = mon->iv;
        heldItem = mon->heldItem;
        MapViewer_GetDefaultMoves(species, level, moves);
        break;
    }
    default:
    {
        const struct RomTrainerMonItemCustomMoves *mon = &((const struct RomTrainerMonItemCustomMoves *)trainer->party.ItemCustomMoves)[slot];
        species = mon->species;
        level = mon->lvl;
        iv = mon->iv;
        heldItem = mon->heldItem;
        memcpy(moves, mon->moves, sizeof(moves));
        break;
    }
    }

    // Species, level, IV (trainer parties give the same IV for every stat, stored as 0-255) and held item
    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, TRAINER_PANEL_TEXT_X, y + 2, sPanelTextColors, 0, gSpeciesNames[species]);

    str = StringCopy(gStringVar4, gText_MapViewer_Lv);
    ConvertIntToDecimalStringN(str, level, STR_CONV_MODE_LEFT_ALIGN, 3);
    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, TRAINER_PANEL_TEXT_X + 58, y + 2, sPanelTextColors, 0, gStringVar4);

    str = StringCopy(gStringVar4, gText_MapViewer_IV);
    *str++ = CHAR_SPACE; // The string compiler drops trailing spaces
    ConvertIntToDecimalStringN(str, iv * 31 / 255, STR_CONV_MODE_LEFT_ALIGN, 2);
    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, TRAINER_PANEL_TEXT_X + 88, y + 2, sPanelTextColors, 0, gStringVar4);

    AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL, TRAINER_PANEL_TEXT_X + 120, y + 2, sPanelTextColors, 0,
                                 heldItem != ITEM_NONE ? ItemId_GetName(heldItem) : gText_MapViewer_NoItem);

    // Moves, two per line
    for (i = 0; i < MAX_MON_MOVES; i++)
        AddTextPrinterParameterized3(WIN_PANEL, FONT_SMALL,
                                     TRAINER_PANEL_TEXT_X + (i % 2) * TRAINER_PANEL_MOVE_W,
                                     y + 2 + TRAINER_PANEL_LINE_H * (1 + i / 2),
                                     sPanelTextColors, 0,
                                     moves[i] != MOVE_NONE ? gMoveNames[moves[i]] : gText_MapViewer_NoMove);

    MapViewer_CreatePanelIcon(extra, species, 18, y + 22);
}

static void MapViewer_DrawTrainerPanel(u8 taskId)
{
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    u8 numTrainers = extra->numObjectTrainers[extra->trainerObject];
    const struct Trainer *trainer = &gTrainers[extra->objectTrainers[extra->trainerObject][extra->trainerPage]];
    u8 *str;
    u8 i;

    MapViewer_DestroyPanelIcons(extra);
    FillWindowPixelBuffer(WIN_PANEL, PIXEL_FILL(1));

    // Header: class and name on the left; double battle, and which of the object's trainers this is, on the right
    str = StringCopy(gStringVar4, gTrainerClassNames[trainer->trainerClass]);
    *str++ = CHAR_SPACE;
    // Same as the battle text: the rival's teams use the name the player gave them
    if (trainer->trainerClass == TRAINER_CLASS_RIVAL_EARLY || trainer->trainerClass == TRAINER_CLASS_RIVAL_LATE
     || trainer->trainerClass == TRAINER_CLASS_CHAMPION)
        StringCopy(str, (const u8 *)gSaveBlock1Ptr + SAVEBLOCK1_RIVAL_NAME_OFFSET);
    else
        StringCopy(str, trainer->trainerName);
    AddTextPrinterParameterized3(WIN_PANEL, FONT_NORMAL, 4, 1, sPanelTextColors, 0, gStringVar4);

    str = gStringVar4;
    *str = EOS;
    if (trainer->doubleBattle)
        str = StringCopy(str, gText_MapViewer_Double);
    if (numTrainers > 1)
    {
        if (str != gStringVar4)
            *str++ = CHAR_SPACE;
        str = ConvertIntToDecimalStringN(str, extra->trainerPage + 1, STR_CONV_MODE_LEFT_ALIGN, 1);
        *str++ = CHAR_SLASH;
        str = ConvertIntToDecimalStringN(str, numTrainers, STR_CONV_MODE_LEFT_ALIGN, 1);
        *str++ = CHAR_SPACE;
        str = StringCopy(str, gText_MapViewer_DpadLeftRight);
    }
    if (trainer->partySize > TRAINER_PANEL_ROWS)
    {
        if (str != gStringVar4)
            *str++ = CHAR_SPACE;
        StringCopy(str, gText_MapViewer_DpadUpDown); // More of the party on the other page
    }
    if (gStringVar4[0] != EOS)
        AddTextPrinterParameterized3(WIN_PANEL, FONT_NORMAL, DISPLAY_WIDTH - 4 - GetStringWidth(FONT_NORMAL, gStringVar4, 0), 1,
                                     sPanelTextColors, 0, gStringVar4);

    for (i = 0; i < TRAINER_PANEL_ROWS; i++)
    {
        u8 slot = extra->trainerPartyPage * TRAINER_PANEL_ROWS + i;

        if (slot >= trainer->partySize || slot >= PARTY_SIZE)
            break;
        MapViewer_DrawTrainerMon(extra, trainer, slot, i);
    }

    PutWindowTilemap(WIN_PANEL);
    CopyWindowToVram(WIN_PANEL, COPYWIN_FULL);
}

static void MapViewer_ShowPanel(u8 taskId, u8 panel)
{
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    u8 i;

    extra->panel = panel;

    // Hide the map's sprites (objects and cursor). HidePanel's MapViewer_UpdateSprites shows them again.
    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse && gSprites[i].sOnMap == MAP_SPRITE_TAG)
            gSprites[i].invisible = TRUE;
    }

    // Busy maps can use nearly every sprite palette slot for NPCs, leaving none for the icons.
    // With the map's sprites hidden, the icons can have any slot; HidePanel puts the map's palettes back.
    memcpy(extra->savedObjPalsUnfaded, &gPlttBufferUnfaded[OBJ_PLTT_OFFSET], OBJ_PLTT_SIZE);
    memcpy(extra->savedObjPalsFaded, &gPlttBufferFaded[OBJ_PLTT_OFFSET], OBJ_PLTT_SIZE);
    extra->savedReservedPalCount = gReservedSpritePaletteCount;
    gReservedSpritePaletteCount = 0;

    // Window 0 covers the whole screen: only the (unblended) panel and the icons in front of it are shown
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, DISPLAY_WIDTH));
    SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(0, DISPLAY_HEIGHT));
    SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG0 | WININ_WIN0_OBJ);
    SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN0_ON);

    if (panel == PANEL_WILD)
        MapViewer_DrawWildPanel(taskId);
    else
        MapViewer_DrawTrainerPanel(taskId);
}

static void MapViewer_HidePanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);

    MapViewer_DestroyPanelIcons(extra);
    extra->panel = PANEL_NONE;

    // Put back the palettes the icons were drawn over (see ShowPanel)
    gReservedSpritePaletteCount = extra->savedReservedPalCount;
    memcpy(&gPlttBufferUnfaded[OBJ_PLTT_OFFSET], extra->savedObjPalsUnfaded, OBJ_PLTT_SIZE);
    memcpy(&gPlttBufferFaded[OBJ_PLTT_OFFSET], extra->savedObjPalsFaded, OBJ_PLTT_SIZE);

    MapViewer_RefreshOverlay(taskId); // Clears the panel, and puts back the highlights, info box and window 0
    MapViewer_UpdateSprites(tScrollX, tScrollY); // Shows the cursor again
}

// A panel is up for as long as its button is held: L for wild encounters, R on a trainer for their party.
// L and R are read raw, so they work the same with the "L=A" button mode.
// Returns TRUE while a panel is up, so other input is ignored.
static bool8 MapViewer_HandlePanels(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct MapViewerExtra *extra = (struct MapViewerExtra *)GetWordTaskArg(taskId, tExtra);
    u8 wanted = PANEL_NONE;
    u8 trainerObject = 0xFF;

    if (JOY_HELD_RAW(L_BUTTON))
        wanted = PANEL_WILD;
    else if (JOY_HELD_RAW(R_BUTTON) && (extra->panel == PANEL_TRAINER || (trainerObject = MapViewer_GetTrainerObjectAtCursor(taskId)) != 0xFF))
        wanted = PANEL_TRAINER;

    if (wanted != extra->panel)
    {
        if (extra->panel != PANEL_NONE)
            MapViewer_HidePanel(taskId);
        if (wanted == PANEL_TRAINER)
        {
            extra->trainerObject = trainerObject;
            extra->trainerPage = 0;
            extra->trainerPartyPage = 0;
        }
        if (wanted != PANEL_NONE)
            MapViewer_ShowPanel(taskId, wanted);
    }
    else if (wanted != PANEL_NONE && (JOY_NEW(DPAD_LEFT) || JOY_NEW(DPAD_RIGHT)))
    {
        s8 direction = JOY_NEW(DPAD_LEFT) ? -1 : 1;

        if (wanted == PANEL_WILD)
        {
            s16 oldMethod = tWildMethod;

            MapViewer_SelectWildMethod(taskId, direction);
            if (tWildMethod != oldMethod)
            {
                PlaySE(SE_SELECT);
                MapViewer_DrawWildPanel(taskId);
            }
        }
        else
        {
            u8 numTrainers = extra->numObjectTrainers[extra->trainerObject];

            if (numTrainers > 1)
            {
                extra->trainerPage = (extra->trainerPage + numTrainers + direction) % numTrainers;
                extra->trainerPartyPage = 0;
                PlaySE(SE_SELECT);
                MapViewer_DrawTrainerPanel(taskId);
            }
        }
    }
    else if (wanted == PANEL_TRAINER && (JOY_NEW(DPAD_UP) || JOY_NEW(DPAD_DOWN)))
    {
        const struct Trainer *trainer = &gTrainers[extra->objectTrainers[extra->trainerObject][extra->trainerPage]];

        if (trainer->partySize > TRAINER_PANEL_ROWS)
        {
            extra->trainerPartyPage ^= 1; // Two pages: party members 1-3 and 4-6
            PlaySE(SE_SELECT);
            MapViewer_DrawTrainerPanel(taskId);
        }
    }

    return (wanted != PANEL_NONE);
}

static void Task_MapViewer(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    const struct MapLayout *layout = header->mapLayout;
    s16 cursorX = tCursorX;
    s16 cursorY = tCursorY;
    s16 scrollX = tScrollX;
    s16 scrollY = tScrollY;

    if (gPaletteFade.active)
        return;

    if (MapViewer_HandlePanels(taskId))
        return;

    if (JOY_NEW(B_BUTTON))
    {
        PlaySE(SE_SELECT);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = Task_MapViewerExit;
        return;
    }

    if (JOY_NEW(A_BUTTON) && MapViewer_TryFollowWarp(taskId, MapViewer_GetWarpAt(header, tCursorX, tCursorY)))
        return;

    if (JOY_REPT(DPAD_LEFT) && cursorX > 0)
        cursorX--;
    else if (JOY_REPT(DPAD_RIGHT) && cursorX < layout->width - 1)
        cursorX++;
    if (JOY_REPT(DPAD_UP) && cursorY > 0)
        cursorY--;
    else if (JOY_REPT(DPAD_DOWN) && cursorY < layout->height - 1)
        cursorY++;

    if (cursorX == tCursorX && cursorY == tCursorY)
        return;

    // Scroll only as far as needed to keep the cursor on screen
    if (cursorX < scrollX)
        scrollX = cursorX;
    else if (cursorX >= scrollX + MAP_VIEWER_WIDTH)
        scrollX = cursorX - MAP_VIEWER_WIDTH + 1;
    if (cursorY < scrollY)
        scrollY = cursorY;
    else if (cursorY >= scrollY + MAP_VIEWER_HEIGHT)
        scrollY = cursorY - MAP_VIEWER_HEIGHT + 1;

    tCursorX = cursorX;
    tCursorY = cursorY;
    if (scrollX != tScrollX || scrollY != tScrollY)
    {
        tScrollX = scrollX;
        tScrollY = scrollY;
        MapViewer_Redraw(header, scrollX, scrollY);
    }
    MapViewer_RefreshOverlay(taskId);
    MapViewer_UpdateSprites(scrollX, scrollY);
}

static void Task_MapViewerExit(u8 taskId)
{
    MainCallback returnCallback;

    if (gPaletteFade.active)
        return;

    returnCallback = (MainCallback)GetWordTaskArg(taskId, tCallback);
    FreeAllWindowBuffers(); // Doesn't free the BG tilemap buffers below, since they were set before InitWindows
    Free((void *)GetWordTaskArg(taskId, tExtra));
    Free(GetBgTilemapBuffer(BG_OVERLAY));
    Free(GetBgTilemapBuffer(BG_TOP));
    Free(GetBgTilemapBuffer(BG_MIDDLE));
    Free(GetBgTilemapBuffer(BG_BOTTOM));
    SetBgTilemapBuffer(BG_OVERLAY, NULL);
    SetBgTilemapBuffer(BG_TOP, NULL);
    SetBgTilemapBuffer(BG_MIDDLE, NULL);
    SetBgTilemapBuffer(BG_BOTTOM, NULL);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    DestroyTask(taskId);
    SetVBlankCallback(NULL);
    SetMainCallback2(returnCallback);
}
