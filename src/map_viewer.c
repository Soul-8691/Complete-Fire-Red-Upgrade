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
#include "../include/map_viewer.h"
#include "../include/constants/songs.h"
#include "../include/constants/rgb.h"
#include "../include/constants/event_object_movement.h"
#include "../include/constants/trainer_types.h"

// From constants/event_objects.h and constants/maps.h, which can't be included here
// (they need the generated map_event_ids.h and map_groups.h)
#define OBJ_EVENT_GFX_VARS 240
#define OBJ_KIND_CLONE     255
#define MAP_DYNAMIC_GROUP  0x7F // MAP_DYNAMIC: a warp back to wherever the player came from
#define MAP_DYNAMIC_NUM    0x7F
#define MAP_UNDEFINED_GROUP 0xFF // Ends gWildMonHeaders

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

static const u32 sHighlightTile[8] =
{
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
    0x11111111, 0x11111111, 0x11111111, 0x11111111,
};

static const u16 sHighlightWarpColor = RGB(31, 31, 0);
static const u16 sHighlightTrainerColor = RGB(31, 0, 0);

// 0: transparent, 1: box background, 2: text, 3: text shadow
static const u16 sInfoTextPal[] = {RGB_BLACK, RGB(2, 2, 4), RGB_WHITE, RGB(11, 11, 13)};
static const u8 sInfoTextColors[] = {1, 2, 3};

static const struct BgTemplate sMapViewerBgTemplates[] =
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
    WIN_WILD_PANEL, // Full screen. Never shown at the same time as the info box, so it reuses its tiles.
    WIN_INFO_NONE = 0xFF,
};

#define INFO_WINDOW_HEIGHT 4 // In tiles

static const struct WindowTemplate sMapViewerWindowTemplates[] =
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
    [WIN_WILD_PANEL] =
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

static const u8 sWildPanelHeaderColors[] = {1, 2, 3};

struct WildPanel
{
    u8 iconSpriteIds[WILD_PANEL_MAX_SLOTS];
    u8 numIcons;
};

// Cursor: 16x16 square outline, black outside and white inside so it shows up on anything
#define TAG_CURSOR 0x4D56

static const u32 sCursorGfx[] =
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

static const u16 sCursorPal[16] = {RGB_BLACK, RGB_WHITE, RGB_BLACK};

static const struct SpriteSheet sCursorSpriteSheet = {sCursorGfx, sizeof(sCursorGfx), TAG_CURSOR};
static const struct SpritePalette sCursorSpritePalette = {sCursorPal, TAG_CURSOR};

static const struct OamData sCursorOamData =
{
    .shape = SPRITE_SHAPE(16x16),
    .size = SPRITE_SIZE(16x16),
    .priority = 0, // In front of all object sprites
};

static const struct SpriteTemplate sCursorSpriteTemplate =
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
#define tWildPanel   11 // Word arg, data[11] and data[12]. NULL while the panel is hidden.
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

extern const u8 gText_MapViewer_WarpInfo[];
extern const u8 gText_MapViewer_DynamicWarp[];
extern const u8 gText_MapViewer_NoWildMons[];
extern const u8 gText_MapViewer_ChangePage[];
extern const u8 gText_MapViewer_Lv[];

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
static bool8 MapViewer_HandleWildPanel(u8 taskId);

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
    SetWordTaskArg(taskId, tWildPanel, (u32)NULL);
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

// Mirrors DrawMetatile in field_camera.c
static void DrawMetatile(u8 layerType, const u16 *tiles, u16 offset)
{
    switch (layerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        DrawMetatileLayer(BG_BOTTOM, offset, tiles);
        DrawMetatileLayer(BG_TOP, offset, tiles + 4);
        break;
    case METATILE_LAYER_TYPE_COVERED:
        DrawMetatileLayer(BG_BOTTOM, offset, tiles);
        DrawMetatileLayer(BG_MIDDLE, offset, tiles + 4);
        break;
    case METATILE_LAYER_TYPE_NORMAL:
    default:
        DrawMetatileLayer(BG_MIDDLE, offset, tiles);
        DrawMetatileLayer(BG_TOP, offset, tiles + 4);
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
static void MapViewer_DrawHighlights(const struct MapHeader *header, s16 scrollX, s16 scrollY)
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

        if (template->objUnion.normal.trainerType != TRAINER_TYPE_NONE && MapViewer_ShouldShowObject(template))
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

    MapViewer_DrawHighlights(header, tScrollX, tScrollY);

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

// Mirrors TrySetupObjectEventSprite in event_object_movement.c
static void MapViewer_CreateObjectSprite(const struct ObjectEventTemplate *template)
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
    // The NPC palettes are already in their fixed slots, only the player and special slots change per object
    if (graphicsInfo->paletteSlot == PALSLOT_PLAYER || graphicsInfo->paletteSlot == PALSLOT_NPC_SPECIAL)
        PatchObjectPalette(graphicsInfo->paletteTag, graphicsInfo->paletteSlot);

    CopyObjectGraphicsInfoToSpriteTemplate(graphicsId, SpriteCallbackDummy, &spriteTemplate, &subspriteTables);
    spriteTemplate.paletteTag = TAG_NONE;
    spriteId = CreateSprite(&spriteTemplate, 0, 0, 0);
    if (spriteId == MAX_SPRITES)
        return;

    sprite = &gSprites[spriteId];
    sprite->centerToCornerVecX = -(graphicsInfo->width >> 1);
    sprite->centerToCornerVecY = -(graphicsInfo->height >> 1);
    sprite->oam.paletteNum = graphicsInfo->paletteSlot;
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
}

static void MapViewer_CreateObjectSprites(const struct MapHeader *header)
{
    u8 i;
    const struct MapEvents *events = header->events;

    InitObjectEventPalettes(0); // Same NPC palette slots the field uses

    if (events == NULL)
        return;

    for (i = 0; i < events->objectEventCount; i++)
    {
        if (MapViewer_ShouldShowObject(&events->objectEvents[i]))
            MapViewer_CreateObjectSprite(&events->objectEvents[i]);
    }
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

static void MapViewer_DestroyWildIcons(struct WildPanel *panel)
{
    u8 i;

    for (i = 0; i < panel->numIcons; i++)
        DestroyMonIcon(&gSprites[panel->iconSpriteIds[i]]);
    panel->numIcons = 0;
    FreeMonIconPalettes();
}

// Prints one cell: the slot's icon, then its level range and chance to its right
static void MapViewer_DrawWildSlot(struct WildPanel *panel, u8 cell, const struct WildPokemon *mon, u8 rate)
{
    u8 spriteId;
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
    AddTextPrinterParameterized3(WIN_WILD_PANEL, FONT_SMALL, x + 36, y + 4, sWildPanelHeaderColors, 0, gStringVar4);

    str = ConvertIntToDecimalStringN(gStringVar4, rate, STR_CONV_MODE_LEFT_ALIGN, 3);
    *str++ = CHAR_PERCENT;
    *str = EOS;
    AddTextPrinterParameterized3(WIN_WILD_PANEL, FONT_SMALL, x + 36, y + 18, sWildPanelHeaderColors, 0, gStringVar4);

    LoadMonIconPalette(mon->species);
    spriteId = CreateMonIcon(mon->species, SpriteCB_MonIcon, x + 18, y + 18, 0, 0, FALSE);
    if (spriteId != MAX_SPRITES)
    {
        gSprites[spriteId].oam.priority = 0; // In front of the panel
        panel->iconSpriteIds[panel->numIcons++] = spriteId;
    }
}

static void MapViewer_DrawWildPanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct WildPanel *panel = (struct WildPanel *)GetWordTaskArg(taskId, tWildPanel);
    const struct WildPokemonHeader *wild = MapViewer_GetWildHeader(tMapGroup, tMapNum);
    u8 i;

    MapViewer_DestroyWildIcons(panel);
    FillWindowPixelBuffer(WIN_WILD_PANEL, PIXEL_FILL(1));

    if (!MapViewer_SelectWildMethod(taskId, 0))
    {
        AddTextPrinterParameterized3(WIN_WILD_PANEL, FONT_NORMAL, 4, 1, sWildPanelHeaderColors, 0, gText_MapViewer_NoWildMons);
    }
    else
    {
        const struct WildMethod *method = &sWildMethods[tWildMethod];
        const struct WildPokemonInfo *info = MapViewer_GetWildInfo(wild, tWildMethod);

        AddTextPrinterParameterized3(WIN_WILD_PANEL, FONT_NORMAL, 4, 1, sWildPanelHeaderColors, 0, method->name);
        if (MapViewer_CountWildMethods(taskId) > 1)
            AddTextPrinterParameterized3(WIN_WILD_PANEL, FONT_NORMAL,
                                         DISPLAY_WIDTH - 4 - GetStringWidth(FONT_NORMAL, gText_MapViewer_ChangePage, 0), 1,
                                         sWildPanelHeaderColors, 0, gText_MapViewer_ChangePage);

        // Every slot is shown separately, even when several have the same species
        for (i = 0; i < method->numSlots; i++)
            MapViewer_DrawWildSlot(panel, i, &info->wildPokemon[method->firstSlot + i], method->slotRates[i]);
    }

    PutWindowTilemap(WIN_WILD_PANEL);
    CopyWindowToVram(WIN_WILD_PANEL, COPYWIN_FULL);
}

static void MapViewer_ShowWildPanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct WildPanel *panel = AllocZeroed(sizeof(struct WildPanel));

    if (panel == NULL)
        return;

    SetWordTaskArg(taskId, tWildPanel, (u32)panel);
    gSprites[tCursorSprite].invisible = TRUE;

    // Window 0 covers the whole screen: only the (unblended) panel and the icons in front of it are shown
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0, DISPLAY_WIDTH));
    SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(0, DISPLAY_HEIGHT));
    SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG0 | WININ_WIN0_OBJ);
    SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_WIN0_ON);

    MapViewer_DrawWildPanel(taskId);
}

static void MapViewer_HideWildPanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct WildPanel *panel = (struct WildPanel *)GetWordTaskArg(taskId, tWildPanel);

    MapViewer_DestroyWildIcons(panel);
    Free(panel);
    SetWordTaskArg(taskId, tWildPanel, (u32)NULL);

    MapViewer_RefreshOverlay(taskId); // Clears the panel, and puts back the highlights, info box and window 0
    MapViewer_UpdateSprites(tScrollX, tScrollY); // Shows the cursor again
}

// The panel is up for as long as L is held. Returns TRUE while it's up, so other input is ignored.
static bool8 MapViewer_HandleWildPanel(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    bool8 shown = (GetWordTaskArg(taskId, tWildPanel) != 0);

    // Raw, so it works the same with the "L=A" button mode
    if (JOY_HELD_RAW(L_BUTTON))
    {
        if (!shown)
        {
            MapViewer_ShowWildPanel(taskId);
        }
        else if (JOY_NEW(DPAD_LEFT) || JOY_NEW(DPAD_RIGHT))
        {
            s16 oldMethod = tWildMethod;

            MapViewer_SelectWildMethod(taskId, JOY_NEW(DPAD_LEFT) ? -1 : 1);
            if (tWildMethod != oldMethod)
            {
                PlaySE(SE_SELECT);
                MapViewer_DrawWildPanel(taskId);
            }
        }
        return TRUE;
    }

    if (shown)
        MapViewer_HideWildPanel(taskId);
    return FALSE;
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

    if (MapViewer_HandleWildPanel(taskId))
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
