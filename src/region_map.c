#include "../include/global.h"
#include "../include/gflib.h"
#include "../include/scanline_effect.h"
#include "../include/task.h"
#include "../include/m4a.h"
#include "../include/overworld.h"
#include "../include/event_data.h"
#include "../include/region_map.h"
#include "../include/party_menu.h"
#include "../include/field_effect.h"
#include "../include/new_menu_helpers.h"
#include "../include/menu.h"
#include "../include/strings.h"
#include "../include/map_preview_screen.h"
#include "../include/constants/songs.h"
#include "../include/constants/region_map_sections.h"
#include "../include/global.fieldmap.h"
#include "../include/fieldmap.h"
#include "../include/bg.h"
#include "../include/window.h"
#include "../include/palette.h"
#include "../include/constants/rgb.h"
#include "../include/map_viewer.h"

#define MAP_WIDTH 22
#define MAP_HEIGHT 15

#define CANCEL_BUTTON_X 21
#define CANCEL_BUTTON_Y 13

#define SWITCH_BUTTON_X 21
#define SWITCH_BUTTON_Y 11

#define REGION_MAP_ICON_COUNT 25

enum {
    REGIONMAP_KANTO,
    REGIONMAP_SEVII123,
    REGIONMAP_SEVII45,
    REGIONMAP_SEVII67,
    REGIONMAP_COUNT
};

enum {
    MAPSECTYPE_NONE,
    MAPSECTYPE_ROUTE,
    MAPSECTYPE_VISITED,
    MAPSECTYPE_NOT_VISITED,
    MAPSECTYPE_UNKNOWN, // Checked but never used
};

enum {
    LAYER_MAP,
    LAYER_DUNGEON,
    LAYER_COUNT
};

enum {
    WIN_MAP_NAME,
    WIN_DUNGEON_NAME,
    WIN_MAP_PREVIEW,
    WIN_TOPBAR_LEFT,
    WIN_TOPBAR_RIGHT,
};

#define CLEAR_NAME 2

enum {
    MAP_INPUT_NONE,
    MAP_INPUT_MOVE_START,
    MAP_INPUT_MOVE_CONT,
    MAP_INPUT_MOVE_END,
    MAP_INPUT_A_BUTTON,
    MAP_INPUT_SWITCH,
    MAP_INPUT_CANCEL
};

enum {
    MAPPERM_HAS_SWITCH_BUTTON,
    MAPPERM_HAS_MAP_PREVIEW,
    MAPPERM_HAS_OPEN_ANIM,
    MAPPERM_HAS_FLY_DESTINATIONS,
    MAPPERM_COUNT
};

enum {
    MAPEDGE_TOP_LEFT,
    MAPEDGE_MID_LEFT,
    MAPEDGE_BOT_LEFT,
    MAPEDGE_TOP_RIGHT,
    MAPEDGE_MID_RIGHT,
    MAPEDGE_BOT_RIGHT,
};

#define FREE_IF_NOT_NULL(ptr) ({ \
    if (ptr) {                   \
        FREE_AND_SET_NULL(ptr);  \
    }                            \
})

struct RegionMap
{
    u8 mapName[19];
    u8 dungeonName[19];
    u16 layouts[REGIONMAP_COUNT + 1][600];
    // Inefficiency: these should be u8 or have half the elements each
    u16 bgTilemapBuffers[3][BG_SCREEN_SIZE];
    u8 type; // REGIONMAP_TYPE_*
    bool8 permissions[MAPPERM_COUNT];
    u8 selectedRegion; // REGIONMAP_KANTO, REGIONMAP_SEVII*
    u8 playersRegion;
    u8 ALIGNED(4) mainState;
    u8 ALIGNED(4) openState;
    u8 ALIGNED(4) loadGfxState;
    u16 dungeonWinLeft;   // Used by a field that's never read
    u16 dungeonWinTop;    // Never read
    u16 dungeonWinRight;  // Never read
    u16 dungeonWinBottom; // Never read
    u8 filler[6]; 
    TaskFunc mainTask;
    MainCallback savedCallback;
}; // size = 0x47C0

struct GpuWindowParams
{
    u16 left;
    u16 top;
    u16 right;
    u16 bottom;
};

struct SwitchMapMenuCursorSubsprite
{
    u8 tiles[0x400];
    struct Sprite *sprite;
    u16 tileTag;
    u16 palTag;
    s16 x;
};

struct SwitchMapMenu
{
    u8 switchMapTiles[0x1000];
    u16 switchMapTilemap[600];
    struct SwitchMapMenuCursorSubsprite cursorSubsprite[2];
    u8 mainState;
    u8 cursorLoadState;
    u8 currentSelection;
    u8 chosenRegion;
    u8 maxSelection;
    u8 alpha;
    u16 yOffset;
    TaskFunc exitTask;
    struct GpuWindowParams highlight;
    u16 blendY;
}; // size = 0x1CE0

struct DungeonMapInfo
{
    u16 id;
    const u8 *name;
    const u8 *desc;
};

struct DungeonMapPreview
{
    u16 tiles[7200];
    u16 tilemap[640];
    const struct MapPreviewScreen * mapPreviewInfo;
    TaskFunc savedTask;
    u8 mainState;
    u8 drawState;
    u8 loadState;
    u8 updateCounter;
    u8 timer;
    u16 palette[0x30];
    u8 filler[0x60];
    u16 red;
    u16 green;
    u16 blue;
    u16 blendY;
    u16 left;
    u16 top;
    u16 right;
    u16 bottom;
    u16 leftIncrement;
    u16 topIncrement;
    u16 rightIncrement;
    u16 bottomIncrement;
}; // size = 0x3E28

struct MapEdge
{
    u16 tiles[0x200];
    struct Sprite *sprite;
    s16 x;
    s16 y;
    u16 tileTag;
    u16 palTag;
};

struct MapOpenCloseAnim
{
    struct MapEdge * mapEdges[6];
    u16 tiles[0x400];
    u16 tilemap[600];
    TaskFunc exitTask;
    u8 openState;
    u8 loadGfxState;
    u8 moveState;
    u8 closeState;
    u8 blendY;
}; // size = 0xCD4

struct MapCursor
{
    s16 x;
    s16 y;
    u16 spriteX;
    u16 spriteY;
    s16 horizontalMove;
    s16 verticalMove;
    u16 moveCounter;
    u8 snapId;
    u8 (*inputHandler)(void);
    u16 selectedMapsec;
    u16 selectedMapsecType;
    u16 selectedDungeonType;
    struct Sprite *sprite;
    u16 tileTag;
    u16 palTag;
    u16 tiles[0x80];
}; // size = 0x124

// --- Field map viewer (town map A button) ---

// RegionMap's filler is unused by the game, so it remembers what's needed
// to reopen the region map where it was after the map viewer closes.
#define sReturningFromViewer filler[0]
#define sSavedCursorX        filler[1]
#define sSavedCursorY        filler[2]
#define sViewerMapsec        filler[3]

extern struct RegionMap *sRegionMap;
extern struct MapCursor *sMapCursor;
extern const u8 sMapFlyDestinations[][3];

extern u8 GetSelectedMapsecType(u8 layer);
extern u16 GetMapsecUnderCursor(void);
extern u16 GetDungeonMapsecUnderCursor(void);
extern bool8 GetRegionMapPermission(u8 attr);
extern u8 GetMapCursorX(void);
extern u8 GetMapCursorY(void);
extern void InitDungeonMapPreview(u8 unused, u8 taskId, TaskFunc savedTask);
extern void SaveMainMapTask(u8 taskId);
extern void SetMapCursorInvisibility(u8 invisible);
extern void SetPlayerIconInvisibility(u8 invisible);
extern void SetFlyIconInvisibility(u8 region, u8 count, u8 invisible);
extern void SetDungeonIconInvisibility(u8 region, u8 count, u8 invisible);
extern void DisplayCurrentMapName(void);
extern void DisplayCurrentDungeonName(void);
extern void DrawDungeonNameBox(void);
extern void PrintTopBarTextRight(const u8 *text);
extern const struct MapHeader *Overworld_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum);
extern void ShowBg(u8 bg);
extern void PlaySE(u16 songId);
extern void InitMapIcons(u8 region, u8 taskId, TaskFunc exitTask);
extern void CreateMapCursor(u8 x, u8 y);
extern void CreatePlayerIcon(u8 x, u8 y);
extern void InitMapOpenAnim(u8 taskId, TaskFunc exitTask);
extern void DoMapCloseAnim(u8 taskId);
extern void FreeRegionMap(u8 taskId);
extern u8 GetRegionMapInput(void);
extern void ResetCursorSnap(void);
extern void PlaySEForSelectedMapsec(void);
extern void InitSwitchMapMenu(u8 region, u8 taskId, TaskFunc exitTask);
extern bool8 IsDma3ManagerBusyWithBgCopy(void);
extern void PrintTopBarTextLeft(const u8 *text);
extern void ClearOrDrawTopBar(bool8 clear);
extern bool8 BeginNormalPaletteFade(u32 selectedPalettes, s8 delay, u8 startY, u8 targetY, u16 blendColor);
extern u8 GetSelectedMapSection(u8 whichMap, u8 layer, s16 y, s16 x);
extern u8 GetMapsecType(u16 mapsec);
extern u8 GetDungeonMapsecType(u16 mapsec);
extern void FreeMapOpenCloseAnim(void);
extern void FreeMapIcons(void);
extern void FreeMapCursor(void);
extern void FreePlayerIcon(void);
extern void FreeAndResetGpuRegs(void);
extern void CB2_OpenRegionMap(void);

extern const u8 gText_RegionMap_AButtonGuide[];
extern const u8 gText_RegionMap_Space[];
extern const u8 gText_RegionMap_AButtonCancel[];
extern const u8 gText_RegionMap_AButtonSwitch[];
extern const u8 gText_RegionMap_DPadMove[];
extern u8 GetSelectedRegionMap(void);

static void RegionMap_PrintTopBarAfterCursorMove(void);
static bool8 RegionMap_CanOpenFieldMapPreview(void);
static const struct MapHeader *RegionMap_GetMapsecHeader(u16 mapsec);
static void RegionMap_TryOpenFieldMapViewer(u8 taskId);
static void RegionMap_RestoreCursorAfterViewer(void);

static void RegionMap_PrintTopBarAfterCursorMove(void)
{
    DisplayCurrentMapName();
    DisplayCurrentDungeonName();
    DrawDungeonNameBox();
    PlaySEForSelectedMapsec();

    if (GetDungeonMapsecUnderCursor() != MAPSEC_NONE)
    {
        if (GetRegionMapPermission(MAPPERM_HAS_MAP_PREVIEW) == TRUE)
        {
            if (GetSelectedMapsecType(LAYER_DUNGEON) == MAPSECTYPE_VISITED)
                PrintTopBarTextRight(gText_RegionMap_AButtonGuide);
            else
                PrintTopBarTextRight(gText_RegionMap_Space);
        }
    }
    else
    {
        if (GetMapCursorX() == SWITCH_BUTTON_X && GetMapCursorY() == SWITCH_BUTTON_Y
            && GetRegionMapPermission(MAPPERM_HAS_SWITCH_BUTTON) == TRUE)
        {
            PrintTopBarTextRight(gText_RegionMap_AButtonSwitch);
        }
        else if (GetMapCursorX() == CANCEL_BUTTON_X && GetMapCursorY() == CANCEL_BUTTON_Y)
        {
            PrintTopBarTextRight(gText_RegionMap_AButtonCancel);
        }
        else if (RegionMap_CanOpenFieldMapPreview())
        {
            PrintTopBarTextRight(gText_RegionMap_AButtonGuide);
        }
        else
        {
            PrintTopBarTextRight(gText_RegionMap_Space);
        }
    }
}

static bool8 RegionMap_CanOpenFieldMapPreview(void)
{
    if (GetMapCursorX() == CANCEL_BUTTON_X && GetMapCursorY() == CANCEL_BUTTON_Y)
        return FALSE;
    if (GetMapCursorX() == SWITCH_BUTTON_X && GetMapCursorY() == SWITCH_BUTTON_Y
        && GetRegionMapPermission(MAPPERM_HAS_SWITCH_BUTTON) == TRUE)
        return FALSE;
    if (GetMapsecUnderCursor() == MAPSEC_NONE)
        return FALSE;
    if (GetSelectedMapsecType(LAYER_MAP) != MAPSECTYPE_VISITED
        && GetSelectedMapsecType(LAYER_MAP) != MAPSECTYPE_ROUTE)
        return FALSE;
    if (RegionMap_GetMapsecHeader(GetMapsecUnderCursor()) == NULL)
        return FALSE;
    return TRUE;
}

void Task_RegionMap_CFRU(u8 taskId)
{
    switch (sRegionMap->mainState)
    {
    case 0:
        InitMapIcons(GetSelectedRegionMap(), taskId, sRegionMap->mainTask);
        CreateMapCursor(0, 0);
        CreatePlayerIcon(1, 1); // Must be before the cursor is moved, it's placed at the cursor
        if (sRegionMap->sReturningFromViewer)
            RegionMap_RestoreCursorAfterViewer();
        sRegionMap->mainState++;
        break;
    case 1:
        if (sRegionMap->permissions[MAPPERM_HAS_OPEN_ANIM] == TRUE)
        {
            InitMapOpenAnim(taskId, sRegionMap->mainTask);
        }
        else
        {
            ShowBg(0);
            ShowBg(3);
            ShowBg(1);
            PrintTopBarTextLeft(gText_RegionMap_DPadMove);
            PrintTopBarTextRight(gText_RegionMap_Space);
            ClearOrDrawTopBar(FALSE);
            SetPlayerIconInvisibility(FALSE);
            SetMapCursorInvisibility(FALSE);
            SetFlyIconInvisibility(GetSelectedRegionMap(), REGION_MAP_ICON_COUNT, FALSE);
            SetDungeonIconInvisibility(GetSelectedRegionMap(), REGION_MAP_ICON_COUNT, FALSE);
        }
        sRegionMap->mainState++;
        break;
    case 2:
        if (!gPaletteFade.active && !IsDma3ManagerBusyWithBgCopy())
        {
            DisplayCurrentMapName();
            PutWindowTilemap(WIN_MAP_NAME);
            DisplayCurrentDungeonName();
            PutWindowTilemap(WIN_DUNGEON_NAME);
            if (sRegionMap->sReturningFromViewer)
            {
                PrintTopBarTextRight(gText_RegionMap_AButtonGuide);
                sRegionMap->sReturningFromViewer = FALSE;
            }
            sRegionMap->mainState++;
        }
        break;
    case 3:
        switch (GetRegionMapInput())
        {
        case MAP_INPUT_MOVE_START:
            ResetCursorSnap();
            break;
        case MAP_INPUT_MOVE_CONT:
            break;
        case MAP_INPUT_MOVE_END:
            RegionMap_PrintTopBarAfterCursorMove();
            break;
        case MAP_INPUT_A_BUTTON:
            if (GetSelectedMapsecType(LAYER_DUNGEON) == MAPSECTYPE_VISITED
                && sRegionMap->permissions[MAPPERM_HAS_MAP_PREVIEW] == TRUE)
            {
                InitDungeonMapPreview(0, taskId, SaveMainMapTask);
            }
            else if (RegionMap_CanOpenFieldMapPreview())
            {
                RegionMap_TryOpenFieldMapViewer(taskId);
            }
            break;
        case MAP_INPUT_SWITCH:
            InitSwitchMapMenu(sRegionMap->selectedRegion, taskId, SaveMainMapTask);
            break;
        case MAP_INPUT_CANCEL:
            sRegionMap->mainState++;
            break;
        }
        break;
    case 4:
        if (GetRegionMapPermission(MAPPERM_HAS_OPEN_ANIM) == TRUE)
        {
            DoMapCloseAnim(taskId);
            goto INCREMENT_MAIN_STATE_AND_BREAK;
        }
        else
        {
            sRegionMap->mainState++;
        }
        break;
    case 5:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    INCREMENT_MAIN_STATE_AND_BREAK:
        sRegionMap->mainState++;
        break;
    default:
        if (!gPaletteFade.active)
            FreeRegionMap(taskId);
        break;
    }
}

static const struct MapHeader *RegionMap_GetMapsecHeader(u16 mapsec)
{
    u16 idx;
    const struct MapHeader *header;

    if (mapsec < KANTO_MAPSEC_START || mapsec >= MAPSEC_COUNT)
        return NULL;

    idx = mapsec - KANTO_MAPSEC_START;
    header = Overworld_GetMapHeaderByGroupAndId(sMapFlyDestinations[idx][0], sMapFlyDestinations[idx][1]);
    if (header == NULL || header->mapLayout == NULL)
        return NULL;

    return header;
}

static void CB2_RegionMap_ReturnFromFieldMapViewer(void)
{
    // Rebuild the region map from scratch, same as when it was first opened
    sRegionMap->sReturningFromViewer = TRUE;
    sRegionMap->mainState = 0;
    sRegionMap->openState = 0;
    sRegionMap->loadGfxState = 0;
    SetMainCallback2(CB2_OpenRegionMap);
}

static void CB2_RegionMap_OpenFieldMapViewer(void)
{
    OpenMapViewer(RegionMap_GetMapsecHeader(sRegionMap->sViewerMapsec), CB2_RegionMap_ReturnFromFieldMapViewer);
}

static void Task_RegionMap_WaitFadeThenOpenViewer(u8 taskId)
{
    if (gPaletteFade.active)
        return;

    // Same as FreeRegionMap, except sRegionMap is kept so the region map can be reopened
    if (GetRegionMapPermission(MAPPERM_HAS_OPEN_ANIM) == TRUE)
        FreeMapOpenCloseAnim();
    FreeMapIcons();
    FreeMapCursor();
    FreePlayerIcon();
    FreeAndResetGpuRegs();
    DestroyTask(taskId);
    FreeAllWindowBuffers();
    SetMainCallback2(CB2_RegionMap_OpenFieldMapViewer);
}

static void RegionMap_TryOpenFieldMapViewer(u8 taskId)
{
    u16 mapsec = GetMapsecUnderCursor();

    if (RegionMap_GetMapsecHeader(mapsec) == NULL)
        return;

    sRegionMap->sViewerMapsec = mapsec;
    sRegionMap->sSavedCursorX = GetMapCursorX();
    sRegionMap->sSavedCursorY = GetMapCursorY();
    PlaySE(SE_SELECT);
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    gTasks[taskId].func = Task_RegionMap_WaitFadeThenOpenViewer;
}

// CreateMapCursor always starts on the player, so move it back to where it was
static void RegionMap_RestoreCursorAfterViewer(void)
{
    s16 x = sRegionMap->sSavedCursorX;
    s16 y = sRegionMap->sSavedCursorY;

    sMapCursor->x = x;
    sMapCursor->y = y;
    sMapCursor->spriteX = 8 * x + 36;
    sMapCursor->spriteY = 8 * y + 36;
    sMapCursor->selectedMapsec = GetSelectedMapSection(GetSelectedRegionMap(), LAYER_MAP, y, x);
    sMapCursor->selectedMapsecType = GetMapsecType(sMapCursor->selectedMapsec);
    sMapCursor->selectedDungeonType = GetDungeonMapsecType(GetSelectedMapSection(GetSelectedRegionMap(), LAYER_DUNGEON, y, x));
    if (sMapCursor->sprite != NULL)
    {
        sMapCursor->sprite->x = sMapCursor->spriteX;
        sMapCursor->sprite->y = sMapCursor->spriteY;
    }
}

struct PlayerIcon
{
    s16 x;
    s16 y;
    struct Sprite *sprite;
    u16 tileTag;
    u16 palTag;
    u16 tiles[0x40];
}; // size = 0x8C

struct MapIconSprite
{
    u32 unused;
    u8 region;
    struct Sprite *sprite;
    u16 tileTag;
    u16 palTag;
};

struct MapIcons
{
    u8 dungeonIconTiles[0x40];
    u8 flyIconTiles[0x100];
    struct MapIconSprite dungeonIcons[25];
    struct MapIconSprite flyIcons[25];
    u8 region; // Never read
    u8 unused_1[2];
    u8 state;
    u32 unused_2;
    TaskFunc exitTask;
}; // size = 0x46C

struct RegionMapGpuRegs
{
    u16 bldcnt;
    u16 bldy;
    u16 bldalpha;
    u16 winin;
    u16 winout;
    u16 win0h;
    u16 win1h;
    u16 win0v;
    u16 win1v;
};

struct FlyMap
{
    u8 state;
    u8 unused; // Never read
    bool8 selectedDestination;
};
