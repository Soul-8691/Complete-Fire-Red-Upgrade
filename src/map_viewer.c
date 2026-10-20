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

// Viewer that draws a map's layout and object events (as seen in a map editor).
// D-Pad moves the cursor (the view follows it), B fades out and switches to the return callback.
// When the cursor is on a warp, the map it leads to is shown in a text box.

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
    DUMMY_WIN_TEMPLATE
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

// Sprite data, for both object sprites and the cursor
#define sMapX      data[0]
#define sMapY      data[1]
#define sElevation data[2]

extern u8 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *mapLayout, u16 metatile, u8 attributeType);
extern const u8 gInitialMovementTypeFacingDirections[];
extern const u8 sElevationToPriority[];
extern const u8 sElevationToSubspriteTableNum[];

extern const u8 gText_MapViewer_WarpInfo[];
extern const u8 gText_MapViewer_DynamicWarp[];

static void Task_WaitFadeThenOpenCurrentMapViewer(u8 taskId);
static void CB2_OpenCurrentMapViewer(void);
static void CB2_MapViewer(void);
static void VBlankCB_MapViewer(void);
static void Task_MapViewerInit(u8 taskId);
static void Task_MapViewer(u8 taskId);
static void Task_MapViewerExit(u8 taskId);
static void MapViewer_Redraw(const struct MapHeader *header, s16 scrollX, s16 scrollY);
static void MapViewer_RefreshOverlay(u8 taskId);
static bool8 MapViewer_ShouldShowObject(const struct ObjectEventTemplate *template);
static void MapViewer_CreateObjectSprites(const struct MapHeader *header);
static void MapViewer_UpdateSprites(s16 scrollX, s16 scrollY);

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
    OpenMapViewer(&gMapHeader, CB2_ReturnToFieldContinueScript);
}

void OpenMapViewer(const struct MapHeader *header, MainCallback returnCallback)
{
    u8 taskId;

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

static void Task_MapViewerInit(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapHeader *header = (const struct MapHeader *)GetWordTaskArg(taskId, tHeader);
    const struct MapLayout *layout = header->mapLayout;

    switch (tState)
    {
    case 0:
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
        SetGpuReg(REG_OFFSET_BLDCNT, 0);
        tState++;
        break;
    case 1:
        CopyMapTilesetsToVram(layout);
        LoadMapTilesetPalettes(layout);
        LoadBgTiles(BG_OVERLAY, sHighlightTile, sizeof(sHighlightTile), HIGHLIGHT_TILE);
        LoadPalette(&sHighlightWarpColor, BG_PLTT_ID(PALSLOT_HIGHLIGHT_WARP) + 1, sizeof(u16));
        LoadPalette(&sHighlightTrainerColor, BG_PLTT_ID(PALSLOT_HIGHLIGHT_TRAINER) + 1, sizeof(u16));
        LoadPalette(sInfoTextPal, BG_PLTT_ID(PALSLOT_INFO_TEXT), sizeof(sInfoTextPal));
        tState++;
        break;
    case 2:
        tScrollX = 0; // Start at the top left corner
        tScrollY = 0;
        tCursorX = 0;
        tCursorY = 0;
        tInfoWindow = WIN_INFO_NONE;
        MapViewer_Redraw(header, tScrollX, tScrollY);
        tState++;
        break;
    case 3:
        MapViewer_CreateObjectSprites(header);
        LoadSpriteSheet(&sCursorSpriteSheet);
        LoadSpritePalette(&sCursorSpritePalette);
        tCursorSprite = CreateSprite(&sCursorSpriteTemplate, 0, 0, 0);
        MapViewer_RefreshOverlay(taskId); // Also positions the cursor
        MapViewer_UpdateSprites(tScrollX, tScrollY);
        tState++;
        break;
    case 4:
        if (IsDma3ManagerBusyWithBgCopy())
            break;
        SetGpuReg(REG_OFFSET_DISPCNT, GetGpuReg(REG_OFFSET_DISPCNT) | DISPCNT_MODE_0 | DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
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
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        SetVBlankCallback(VBlankCB_MapViewer);
        gTasks[taskId].func = Task_MapViewer;
        break;
    }
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

        if (!sprite->inUse)
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

    if (JOY_NEW(B_BUTTON))
    {
        PlaySE(SE_SELECT);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = Task_MapViewerExit;
        return;
    }

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
