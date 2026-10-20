#include "../include/global.h"
#include "../include/gflib.h"
#include "../include/main.h"
#include "../include/new_menu_helpers.h"
#include "../include/task.h"
#include "../include/scanline_effect.h"
#include "../include/overworld.h"
#include "../include/fieldmap.h"
#include "../include/global.fieldmap.h"
#include "../include/event_data.h"
#include "../include/event_object_movement.h"
#include "../include/map_viewer.h"
#include "../include/constants/songs.h"
#include "../include/constants/rgb.h"
#include "../include/constants/event_object_movement.h"

// From constants/event_objects.h, which can't be included here (needs generated map_event_ids.h)
#define OBJ_EVENT_GFX_VARS 240
#define OBJ_KIND_CLONE     255

// Viewer that draws a map's layout and object events (as seen in a map editor).
// D-Pad scrolls, B fades out and switches to the return callback.

#define MAP_VIEWER_WIDTH  15 // Screen size in metatiles
#define MAP_VIEWER_HEIGHT 10
#define MAP_VIEWER_TILEMAP_SIZE (32 * 32 * sizeof(u16))

// Same layer arrangement as the overworld (see sOverworldBgTemplates).
// All three share char block 0, which is where CopyMapTilesetsToVram puts the tileset (via BG2).
#define BG_TOP    1
#define BG_MIDDLE 2
#define BG_BOTTOM 3

static const struct BgTemplate sMapViewerBgTemplates[] =
{
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

// Task data
#define tState    data[0]
#define tScrollX  data[1]
#define tScrollY  data[2]
#define tHeader   3 // Word arg, data[3] and data[4]
#define tCallback 5 // Word arg, data[5] and data[6]

// Object sprite data
#define sMapX      data[0]
#define sMapY      data[1]
#define sElevation data[2]

extern u8 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *mapLayout, u16 metatile, u8 attributeType);
extern const u8 gInitialMovementTypeFacingDirections[];
extern const u8 sElevationToPriority[];
extern const u8 sElevationToSubspriteTableNum[];

static void Task_WaitFadeThenOpenCurrentMapViewer(u8 taskId);
static void CB2_OpenCurrentMapViewer(void);
static void CB2_MapViewer(void);
static void VBlankCB_MapViewer(void);
static void Task_MapViewerInit(u8 taskId);
static void Task_MapViewer(u8 taskId);
static void Task_MapViewerExit(u8 taskId);
static void MapViewer_Redraw(const struct MapLayout *layout, s16 scrollX, s16 scrollY);
static void MapViewer_CreateObjectSprites(const struct MapHeader *header);
static void MapViewer_UpdateObjectSprites(s16 scrollX, s16 scrollY);

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
        ChangeBgX(BG_TOP, 0, BG_COORD_SET);
        ChangeBgY(BG_TOP, 0, BG_COORD_SET);
        ChangeBgX(BG_MIDDLE, 0, BG_COORD_SET);
        ChangeBgY(BG_MIDDLE, 0, BG_COORD_SET);
        ChangeBgX(BG_BOTTOM, 0, BG_COORD_SET);
        ChangeBgY(BG_BOTTOM, 0, BG_COORD_SET);
        SetGpuReg(REG_OFFSET_BLDCNT, 0);
        tState++;
        break;
    case 1:
        CopyMapTilesetsToVram(layout);
        LoadMapTilesetPalettes(layout);
        tState++;
        break;
    case 2:
        tScrollX = 0; // Start at the top left corner
        tScrollY = 0;
        MapViewer_Redraw(layout, tScrollX, tScrollY);
        tState++;
        break;
    case 3:
        MapViewer_CreateObjectSprites(header);
        MapViewer_UpdateObjectSprites(tScrollX, tScrollY);
        tState++;
        break;
    case 4:
        if (IsDma3ManagerBusyWithBgCopy())
            break;
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
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

static void MapViewer_Redraw(const struct MapLayout *layout, s16 scrollX, s16 scrollY)
{
    s16 x, y;

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

// Every sprite in the viewer is an object sprite, so just move them all
static void MapViewer_UpdateObjectSprites(s16 scrollX, s16 scrollY)
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
    s16 maxX = max(0, layout->width - MAP_VIEWER_WIDTH);
    s16 maxY = max(0, layout->height - MAP_VIEWER_HEIGHT);
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

    if (JOY_REPT(DPAD_LEFT) && scrollX > 0)
        scrollX--;
    else if (JOY_REPT(DPAD_RIGHT) && scrollX < maxX)
        scrollX++;
    if (JOY_REPT(DPAD_UP) && scrollY > 0)
        scrollY--;
    else if (JOY_REPT(DPAD_DOWN) && scrollY < maxY)
        scrollY++;

    if (scrollX != tScrollX || scrollY != tScrollY)
    {
        tScrollX = scrollX;
        tScrollY = scrollY;
        MapViewer_Redraw(layout, scrollX, scrollY);
        MapViewer_UpdateObjectSprites(scrollX, scrollY);
    }
}

static void Task_MapViewerExit(u8 taskId)
{
    MainCallback returnCallback;

    if (gPaletteFade.active)
        return;

    returnCallback = (MainCallback)GetWordTaskArg(taskId, tCallback);
    Free(GetBgTilemapBuffer(BG_TOP));
    Free(GetBgTilemapBuffer(BG_MIDDLE));
    Free(GetBgTilemapBuffer(BG_BOTTOM));
    SetBgTilemapBuffer(BG_TOP, NULL);
    SetBgTilemapBuffer(BG_MIDDLE, NULL);
    SetBgTilemapBuffer(BG_BOTTOM, NULL);
    DestroyTask(taskId);
    SetVBlankCallback(NULL);
    SetMainCallback2(returnCallback);
}
