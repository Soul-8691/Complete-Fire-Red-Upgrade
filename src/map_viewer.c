#include "../include/global.h"
#include "../include/gflib.h"
#include "../include/main.h"
#include "../include/new_menu_helpers.h"
#include "../include/task.h"
#include "../include/scanline_effect.h"
#include "../include/overworld.h"
#include "../include/fieldmap.h"
#include "../include/global.fieldmap.h"
#include "../include/map_viewer.h"
#include "../include/constants/songs.h"
#include "../include/constants/rgb.h"

// Viewer that draws a map layout (as seen in a map editor).
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
#define tLayout   3 // Word arg, data[3] and data[4]
#define tCallback 5 // Word arg, data[5] and data[6]

extern u8 GetAttributeByMetatileIdAndMapLayout(const struct MapLayout *mapLayout, u16 metatile, u8 attributeType);

static void Task_WaitFadeThenOpenCurrentMapViewer(u8 taskId);
static void CB2_OpenCurrentMapViewer(void);
static void CB2_MapViewer(void);
static void VBlankCB_MapViewer(void);
static void Task_MapViewerInit(u8 taskId);
static void Task_MapViewer(u8 taskId);
static void Task_MapViewerExit(u8 taskId);
static void MapViewer_Redraw(const struct MapLayout *layout, s16 scrollX, s16 scrollY);

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
    OpenMapViewer(gMapHeader.mapLayout, CB2_ReturnToFieldContinueScript);
}

void OpenMapViewer(const struct MapLayout *layout, MainCallback returnCallback)
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
    SetWordTaskArg(taskId, tLayout, (u32)layout);
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
    const struct MapLayout *layout = (const struct MapLayout *)GetWordTaskArg(taskId, tLayout);

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
        if (IsDma3ManagerBusyWithBgCopy())
            break;
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_1D_MAP);
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

static void Task_MapViewer(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const struct MapLayout *layout = (const struct MapLayout *)GetWordTaskArg(taskId, tLayout);
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
