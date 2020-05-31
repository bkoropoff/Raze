//-------------------------------------------------------------------------
/*
Copyright (C) 2016 EDuke32 developers and contributors

This file is part of EDuke32.

EDuke32 is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------

#include "ns.h"	// Must come before everything else!

#define game_c_

#include <chrono>
#include <thread>
#include "duke3d.h"
#include "compat.h"
#include "baselayer.h"
#include "osdcmds.h"
#include "network.h"
#include "menus.h"
#include "savegame.h"
#include "anim.h"
#include "demo.h"

#include "cheats.h"
#include "sbar.h"
#include "screens.h"
#include "cmdline.h"
#include "palette.h"
#include "gamecvars.h"
#include "gameconfigfile.h"
#include "printf.h"
#include "m_argv.h"
#include "c_dispatch.h"
#include "filesystem.h"
#include "statistics.h"
#include "menu/menu.h"
#include "mapinfo.h"
#include "v_video.h"
#include "glbackend/glbackend.h"

// Uncomment to prevent anything except mirrors from drawing. It is sensible to
// also uncomment ENGINE_CLEAR_SCREEN in build/src/engine_priv.h.
//#define DEBUG_MIRRORS_ONLY

#if KRANDDEBUG
# define GAME_INLINE
# define GAME_STATIC
#else
# define GAME_INLINE inline
# define GAME_STATIC static
#endif

BEGIN_DUKE_NS

int32_t g_quitDeadline = 0;

int32_t g_cameraDistance = 0, g_cameraClock = 0;
static int32_t g_quickExit;

char boardfilename[BMAX_PATH];

int32_t voting = -1;
int32_t vote_map = -1, vote_episode = -1;

int32_t g_Debug = 0;

#ifndef EDUKE32_STANDALONE
static const char *defaultrtsfilename[GAMECOUNT] = { "DUKE.RTS", "NAM.RTS", "NAPALM.RTS", "WW2GI.RTS" };
#endif

int32_t g_Shareware = 0;

int32_t tempwallptr;

static int32_t nonsharedtimer;

int32_t ticrandomseed;

int32_t g_levelTextTime = 0;

#if defined(RENDERTYPEWIN) && defined(USE_OPENGL)
extern char forcegl;
#endif

const char *G_DefaultRtsFile(void)
{
#ifndef EDUKE32_STANDALONE
    if (DUKE)
        return defaultrtsfilename[GAME_DUKE];
    else if (WW2GI)
        return defaultrtsfilename[GAME_WW2GI];
    else if (NAPALM)
    {
        if (!fileSystem.FileExists(defaultrtsfilename[GAME_NAPALM]) && fileSystem.FileExists(defaultrtsfilename[GAME_NAM]))
            return defaultrtsfilename[GAME_NAM]; // NAM/NAPALM Sharing
        else
            return defaultrtsfilename[GAME_NAPALM];
    }
    else if (NAM)
    {
        if (!fileSystem.FileExists(defaultrtsfilename[GAME_NAM]) && fileSystem.FileExists(defaultrtsfilename[GAME_NAPALM]))
            return defaultrtsfilename[GAME_NAPALM]; // NAM/NAPALM Sharing
        else
            return defaultrtsfilename[GAME_NAM];
    }
#endif

    return "";
}

enum gametokens
{
    T_INCLUDE = 0,
    T_INTERFACE = 0,
    T_LOADGRP = 1,
    T_MODE = 1,
    T_CACHESIZE = 2,
    T_ALLOW = 2,
    T_NOAUTOLOAD,
    T_INCLUDEDEFAULT,
    T_MUSIC,
    T_SOUND,
    T_FILE,
    T_CUTSCENE,
    T_ANIMSOUNDS,
    T_NOFLOORPALRANGE,
    T_ID,
    T_MINPITCH,
    T_MAXPITCH,
    T_PRIORITY,
    T_TYPE,
    T_DISTANCE,
    T_VOLUME,
    T_DELAY,
    T_RENAMEFILE,
    T_GLOBALGAMEFLAGS,
    T_ASPECT,
    T_FORCEFILTER,
    T_FORCENOFILTER,
    T_TEXTUREFILTER,
    T_NEWGAMECHOICES,
    T_CHOICE,
    T_NAME,
    T_LOCKED,
    T_HIDDEN,
    T_USERCONTENT,
};

static void gameTimerHandler(void)
{
    S_Update();
    G_HandleSpecialKeys();
}


void G_HandleSpecialKeys(void)
{
    auto &myplayer = *g_player[myconnectindex].ps;

    // we need CONTROL_GetInput in order to pick up joystick button presses
    if ((!(myplayer.gm & MODE_GAME) || (myplayer.gm & MODE_MENU)))
    {
        ControlInfo noshareinfo;
        CONTROL_GetInput(&noshareinfo);
    }

    // only dispatch commands here when not in a game
    if ((myplayer.gm & MODE_GAME) != MODE_GAME)
        OSD_DispatchQueued();

}

void G_GameQuit(void)
{
    if (numplayers < 2)
        G_GameExit(" ");

    if (g_gameQuit == 0)
    {
        g_gameQuit = 1;
        g_quitDeadline = (int32_t) totalclock+120;
        g_netDisconnect = 1;
    }

    if ((totalclock > g_quitDeadline) && (g_gameQuit == 1))
        G_GameExit("Timed out.");
}


int32_t A_CheckInventorySprite(spritetype *s)
{
    switch (DYNAMICTILEMAP(s->picnum))
    {
    case FIRSTAID__STATIC:
    case STEROIDS__STATIC:
    case HEATSENSOR__STATIC:
    case BOOTS__STATIC:
    case JETPACK__STATIC:
    case HOLODUKE__STATIC:
    case AIRTANK__STATIC:
        return 1;
    default:
        return 0;
    }
}



void G_GameExit(const char *msg)
{
    if (*msg != 0 && g_player[myconnectindex].ps != NULL)
        g_player[myconnectindex].ps->palette = BASEPAL;

    if (ud.recstat == 1)
        G_CloseDemoWrite();
	else if (ud.recstat == 2)
	{
		delete g_demo_filePtr;
		g_demo_filePtr = nullptr;
	}
    // JBF: fixes crash on demo playback
    // PK: modified from original

    if (!g_quickExit)
    {
        if (VM_OnEventWithReturn(EVENT_EXITGAMESCREEN, g_player[myconnectindex].ps->i, myconnectindex, 0) == 0 &&
           g_mostConcurrentPlayers > 1 && g_player[myconnectindex].ps->gm & MODE_GAME && GTFLAGS(GAMETYPE_SCORESHEET) && *msg == ' ')
        {
            G_BonusScreen(1);
        }

        // shareware and TEN screens
        if (VM_OnEventWithReturn(EVENT_EXITPROGRAMSCREEN, g_player[myconnectindex].ps->i, myconnectindex, 0) == 0 &&
           *msg != 0 && *(msg+1) != 'V' && *(msg+1) != 'Y')
            G_DisplayExtraScreens();
    }

    if (*msg != 0)
    {
        if (!(msg[0] == ' ' && msg[1] == 0))
        {
			I_FatalError("%s", msg);
        }
    }
	throw CExitEvent(0);
}


static int32_t G_DoThirdPerson(const DukePlayer_t *pp, vec3_t *vect, int16_t *vsectnum, int16_t ang, int16_t horiz)
{
    auto const sp = &sprite[pp->i];
    int32_t i, hx, hy;
    int32_t bakcstat = sp->cstat;
    hitdata_t hit;

    vec3_t n = {
        sintable[(ang+1536)&2047]>>4,
        sintable[(ang+1024)&2047]>>4,
        (horiz-100) * 128
    };

    updatesectorz(vect->x,vect->y,vect->z,vsectnum);

    sp->cstat &= ~0x101;
    hitscan(vect, *vsectnum, n.x,n.y,n.z, &hit, CLIPMASK1);
    sp->cstat = bakcstat;

    if (*vsectnum < 0)
        return -1;

    hx = hit.pos.x-(vect->x);
    hy = hit.pos.y-(vect->y);

    if (klabs(n.x)+klabs(n.y) > klabs(hx)+klabs(hy))
    {
        *vsectnum = hit.sect;

        if (hit.wall >= 0)
        {
            int32_t daang = getangle(wall[wall[hit.wall].point2].x-wall[hit.wall].x,
                             wall[wall[hit.wall].point2].y-wall[hit.wall].y);

            i = n.x*sintable[daang] + n.y*sintable[(daang+1536)&2047];

            if (klabs(n.x) > klabs(n.y))
                hx -= mulscale28(n.x,i);
            else hy -= mulscale28(n.y,i);
        }
        else if (hit.sprite < 0)
        {
            if (klabs(n.x) > klabs(n.y))
                hx -= (n.x>>5);
            else hy -= (n.y>>5);
        }

        if (klabs(n.x) > klabs(n.y))
            i = divscale16(hx,n.x);
        else i = divscale16(hy,n.y);

        if (i < CAMERADIST)
            CAMERADIST = i;
    }

    vect->x += mulscale16(n.x,CAMERADIST);
    vect->y += mulscale16(n.y,CAMERADIST);
    vect->z += mulscale16(n.z,CAMERADIST);

    CAMERADIST = min(CAMERADIST+(((int32_t) totalclock-CAMERACLOCK)<<10),65536);
    CAMERACLOCK = (int32_t) totalclock;

    updatesectorz(vect->x,vect->y,vect->z,vsectnum);

    return 0;
}

#ifdef LEGACY_ROR
char ror_protectedsectors[MAXSECTORS];
static int32_t drawing_ror = 0;
static int32_t ror_sprite = -1;

static void G_OROR_DupeSprites(spritetype const *sp)
{
    // dupe the sprites touching the portal to the other sector
    int32_t k;
    spritetype const *refsp;

    if ((unsigned)sp->yvel >= (unsigned)g_mostConcurrentPlayers)
        return;

    refsp = &sprite[sp->yvel];

    for (SPRITES_OF_SECT(sp->sectnum, k))
    {
        if (spritesortcnt >= maxspritesonscreen)
            break;

        if (sprite[k].picnum != SECTOREFFECTOR && sprite[k].z >= sp->z)
        {
            tspriteptr_t tsp = renderAddTSpriteFromSprite(k);
            Duke_ApplySpritePropertiesToTSprite(tsp, (uspriteptr_t)&sprite[k]);

            tsp->x += (refsp->x - sp->x);
            tsp->y += (refsp->y - sp->y);
            tsp->z += -sp->z + actor[sp->yvel].ceilingz;
            tsp->sectnum = refsp->sectnum;

//            Printf("duped sprite of pic %d at %d %d %d\n",tsp->picnum,tsp->x,tsp->y,tsp->z);
        }
    }
}

static int16_t SE40backupStat[MAXSECTORS];
static int32_t SE40backupZ[MAXSECTORS];

static void G_SE40(int32_t smoothratio)
{
    if ((unsigned)ror_sprite < MAXSPRITES)
    {
        int32_t x, y, z;
        int16_t sect;
        int32_t level = 0;
        auto const sp = &sprite[ror_sprite];
        const int32_t sprite2 = sp->yvel;

        if ((unsigned)sprite2 >= MAXSPRITES)
            return;

        if (klabs(sector[sp->sectnum].floorz - sp->z) < klabs(sector[sprite[sprite2].sectnum].floorz - sprite[sprite2].z))
            level = 1;

        x = CAMERA(pos.x) - sp->x;
        y = CAMERA(pos.y) - sp->y;
        z = CAMERA(pos.z) - (level ? sector[sp->sectnum].floorz : sector[sp->sectnum].ceilingz);

        sect = sprite[sprite2].sectnum;
        updatesector(sprite[sprite2].x + x, sprite[sprite2].y + y, &sect);

        if (sect != -1)
        {
            int32_t renderz, picnum;
            // XXX: PK: too large stack allocation for my taste
            int32_t i;
            int32_t pix_diff, newz;
            //                Printf("drawing ror\n");

            if (level)
            {
                // renderz = sector[sprite[sprite2].sectnum].ceilingz;
                renderz = sprite[sprite2].z - (sprite[sprite2].yrepeat * tilesiz[sprite[sprite2].picnum].y<<1);
                picnum = sector[sprite[sprite2].sectnum].ceilingpicnum;
                sector[sprite[sprite2].sectnum].ceilingpicnum = 562;
				tileDelete(562);

                pix_diff = klabs(z) >> 8;
                newz = - ((pix_diff / 128) + 1) * (128<<8);

                for (i = 0; i < numsectors; i++)
                {
                    SE40backupStat[i] = sector[i].ceilingstat;
                    SE40backupZ[i] = sector[i].ceilingz;
                    if (!ror_protectedsectors[i] || sp->lotag == 41)
                    {
                        sector[i].ceilingstat = 1;
                        sector[i].ceilingz += newz;
                    }
                }
            }
            else
            {
                // renderz = sector[sprite[sprite2].sectnum].floorz;
                renderz = sprite[sprite2].z;
                picnum = sector[sprite[sprite2].sectnum].floorpicnum;
                sector[sprite[sprite2].sectnum].floorpicnum = 562;
				tileDelete(562);

                pix_diff = klabs(z) >> 8;
                newz = ((pix_diff / 128) + 1) * (128<<8);

                for (i = 0; i < numsectors; i++)
                {
                    SE40backupStat[i] = sector[i].floorstat;
                    SE40backupZ[i] = sector[i].floorz;
                    if (!ror_protectedsectors[i] || sp->lotag == 41)
                    {
                        sector[i].floorstat = 1;
                        sector[i].floorz = +newz;
                    }
                }
            }

#ifdef POLYMER
            if (videoGetRenderMode() == REND_POLYMER)
                polymer_setanimatesprites(G_DoSpriteAnimations, CAMERA(pos.x), CAMERA(pos.y), CAMERA(pos.z), fix16_to_int(CAMERA(q16ang)), smoothratio);
#endif
            renderDrawRoomsQ16(sprite[sprite2].x + x, sprite[sprite2].y + y,
                      z + renderz, CAMERA(q16ang), CAMERA(q16horiz), sect);
            drawing_ror = 1 + level;

            if (drawing_ror == 2) // viewing from top
                G_OROR_DupeSprites(sp);

            G_DoSpriteAnimations(CAMERA(pos.x),CAMERA(pos.y),CAMERA(pos.z),fix16_to_int(CAMERA(q16ang)),smoothratio);
            renderDrawMasks();

            if (level)
            {
                sector[sprite[sprite2].sectnum].ceilingpicnum = picnum;
                for (i = 0; i < numsectors; i++)
                {
                    sector[i].ceilingstat = SE40backupStat[i];
                    sector[i].ceilingz = SE40backupZ[i];
                }
            }
            else
            {
                sector[sprite[sprite2].sectnum].floorpicnum = picnum;

                for (i = 0; i < numsectors; i++)
                {
                    sector[i].floorstat = SE40backupStat[i];
                    sector[i].floorz = SE40backupZ[i];
                }
            }
        }
    }
}
#endif

void G_HandleMirror(int32_t x, int32_t y, int32_t z, fix16_t a, fix16_t q16horiz, int32_t smoothratio)
{
    if ((gotpic[MIRROR>>3]&pow2char[MIRROR&7])
#ifdef POLYMER
        && (videoGetRenderMode() != REND_POLYMER)
#endif
        )
    {
        if (g_mirrorCount == 0)
        {
            // NOTE: We can have g_mirrorCount==0 but gotpic'd MIRROR,
            // for example in LNGA2.
            gotpic[MIRROR>>3] &= ~pow2char[MIRROR&7];

            //give scripts the chance to reset gotpics for effects that run in EVENT_DISPLAYROOMS
            //EVENT_RESETGOTPICS must be called after the last call to EVENT_DISPLAYROOMS in a frame, but before any engine-side renderDrawRoomsQ16
            VM_OnEvent(EVENT_RESETGOTPICS, -1, -1);
            return;
        }

        int32_t i = 0, dst = INT32_MAX;

        for (bssize_t k=g_mirrorCount-1; k>=0; k--)
        {
            if (!wallvisible(x, y, g_mirrorWall[k]))
                continue;

            const int32_t j =
                klabs(wall[g_mirrorWall[k]].x - x) +
                klabs(wall[g_mirrorWall[k]].y - y);

            if (j < dst)
                dst = j, i = k;
        }

        if (wall[g_mirrorWall[i]].overpicnum != MIRROR)
        {
            // Try to find a new mirror wall in case the original one was broken.

            int32_t startwall = sector[g_mirrorSector[i]].wallptr;
            int32_t endwall = startwall + sector[g_mirrorSector[i]].wallnum;

            for (bssize_t k=startwall; k<endwall; k++)
            {
                int32_t j = wall[k].nextwall;
                if (j >= 0 && (wall[j].cstat&32) && wall[j].overpicnum==MIRROR)  // cmp. premap.c
                {
                    g_mirrorWall[i] = j;
                    break;
                }
            }
        }

        if (wall[g_mirrorWall[i]].overpicnum == MIRROR)
        {
            int32_t tposx, tposy;
            fix16_t tang;

            //prepare to render any scripted EVENT_DISPLAYROOMS extras as mirrored
            renderPrepareMirror(x, y, z, a, q16horiz, g_mirrorWall[i], &tposx, &tposy, &tang);

            int32_t j = g_visibility;
            g_visibility = (j>>1) + (j>>2);

            //backup original camera position
            auto origCam = CAMERA(pos);
            fix16_t origCamq16ang   = CAMERA(q16ang);
            fix16_t origCamq16horiz = CAMERA(q16horiz);

            //set the camera inside the mirror facing out
            CAMERA(pos)      = { tposx, tposy, z };
            CAMERA(q16ang)   = tang;
            CAMERA(q16horiz) = q16horiz;

            display_mirror = 1;
            VM_OnEventWithReturn(EVENT_DISPLAYROOMS, g_player[0].ps->i, 0, 0);
            display_mirror = 0;

            //reset the camera position
            CAMERA(pos)      = origCam;
            CAMERA(q16ang)   = origCamq16ang;
            CAMERA(q16horiz) = origCamq16horiz;

            //give scripts the chance to reset gotpics for effects that run in EVENT_DISPLAYROOMS
            //EVENT_RESETGOTPICS must be called after the last call to EVENT_DISPLAYROOMS in a frame, but before any engine-side renderDrawRoomsQ16
            VM_OnEvent(EVENT_RESETGOTPICS, -1, -1);

            //prepare to render the mirror
            renderPrepareMirror(x, y, z, a, q16horiz, g_mirrorWall[i], &tposx, &tposy, &tang);

            if (videoGetRenderMode() != REND_POLYMER)
            {
                int32_t didmirror;

                yax_preparedrawrooms();
                didmirror = renderDrawRoomsQ16(tposx,tposy,z,tang,q16horiz,g_mirrorSector[i]+MAXSECTORS);
                //POGO: if didmirror == 0, we may simply wish to abort instead of rendering with yax_drawrooms (which may require cleaning yax state)
                if (videoGetRenderMode() != REND_CLASSIC || didmirror)
                    yax_drawrooms(G_DoSpriteAnimations, g_mirrorSector[i], didmirror, smoothratio);
            }
#ifdef USE_OPENGL
            else
                renderDrawRoomsQ16(tposx,tposy,z,tang,q16horiz,g_mirrorSector[i]+MAXSECTORS);
            // XXX: Sprites don't get drawn with TROR/Polymost
#endif
            display_mirror = 1;
            G_DoSpriteAnimations(tposx,tposy,z,fix16_to_int(tang),smoothratio);
            display_mirror = 0;

            renderDrawMasks();
            renderCompleteMirror();   //Reverse screen x-wise in this function
            g_visibility = j;
        }
    }
}

static void G_ClearGotMirror()
{
#ifdef SPLITSCREEN_MOD_HACKS
    if (!g_fakeMultiMode)
#endif
    {
        // HACK for splitscreen mod: this is so that mirrors will be drawn
        // from showview commands. Ugly, because we'll attempt do draw mirrors
        // each frame then. But it's better than not drawing them, I guess.
        // XXX: fix the sequence of setting/clearing this bit. Right now,
        // we always draw one frame without drawing the mirror, after which
        // the bit gets set and drawn subsequently.
        gotpic[MIRROR>>3] &= ~pow2char[MIRROR&7];
    }
}


void G_DrawRooms(int32_t playerNum, int32_t smoothRatio)
{
    auto const pPlayer = g_player[playerNum].ps;

    int const viewingRange = viewingrange;

    if (g_networkMode == NET_DEDICATED_SERVER) return;

    totalclocklock = totalclock;

    if (pub > 0 || videoGetRenderMode() >= REND_POLYMOST) // JBF 20040101: redraw background always
    {
        videoClearScreen(0);
#ifndef EDUKE32_TOUCH_DEVICES
        if (ud.screen_size >= 8)
#endif
            G_DrawBackground();
        pub = 0;
    }

    VM_OnEvent(EVENT_DISPLAYSTART, pPlayer->i, playerNum);

    if ((ud.overhead_on == 2 && !automapping) || (pPlayer->cursectnum == -1 && videoGetRenderMode() != REND_CLASSIC))
        return;

    if (r_usenewaspect)
    {
        newaspect_enable = 1;
        videoSetCorrectedAspect();
    }

    if (pPlayer->on_crane > -1)
        smoothRatio = 65536;

    int const playerVis = pPlayer->visibility;
    g_visibility        = (playerVis <= 0) ? 0 : (int32_t)(playerVis * (numplayers > 1 ? 1.f : r_ambientlightrecip));

    CAMERA(sect) = pPlayer->cursectnum;

    G_DoInterpolations(smoothRatio);
    G_AnimateCamSprite(smoothRatio);

    if (ud.camerasprite >= 0)
    {
        auto const pSprite = &sprite[ud.camerasprite];

        pSprite->yvel = clamp(TrackerCast(pSprite->yvel), -100, 300);

        CAMERA(q16ang) = fix16_from_int(actor[ud.camerasprite].tempang
                                      + mulscale16(((pSprite->ang + 1024 - actor[ud.camerasprite].tempang) & 2047) - 1024, smoothRatio));

#ifdef USE_OPENGL
        renderSetRollAngle(0);
#endif

        int const noDraw = VM_OnEventWithReturn(EVENT_DISPLAYROOMSCAMERA, ud.camerasprite, playerNum, 0);

        if (noDraw != 1)  // event return values other than 0 and 1 are reserved
        {
#ifdef DEBUGGINGAIDS
            if (EDUKE32_PREDICT_FALSE(noDraw != 0))
                Printf(TEXTCOLOR_RED "ERROR: EVENT_DISPLAYROOMSCAMERA return value must be 0 or 1, "
                           "other values are reserved.\n");
#endif

            renderBeginScene();
#ifdef LEGACY_ROR
            G_SE40(smoothRatio);
#endif
#ifdef POLYMER
            if (videoGetRenderMode() == REND_POLYMER)
                polymer_setanimatesprites(G_DoSpriteAnimations, pSprite->x, pSprite->y, pSprite->z - ZOFFSET6, fix16_to_int(CAMERA(q16ang)), smoothRatio);
#endif
            yax_preparedrawrooms();
            renderDrawRoomsQ16(pSprite->x, pSprite->y, pSprite->z - ZOFFSET6, CAMERA(q16ang), fix16_from_int(pSprite->yvel), pSprite->sectnum);
            yax_drawrooms(G_DoSpriteAnimations, pSprite->sectnum, 0, smoothRatio);
            G_DoSpriteAnimations(pSprite->x, pSprite->y, pSprite->z - ZOFFSET6, fix16_to_int(CAMERA(q16ang)), smoothRatio);
            renderDrawMasks();
            renderFinishScene();
        }
    }
    else
    {
        int32_t floorZ, ceilZ;

        int vr            = divscale22(1, sprite[pPlayer->i].yrepeat + 28);

        vr = Blrintf(double(vr) * tan(r_fov * (PI/360.)));

        if (!r_usenewaspect)
            renderSetAspect(vr, yxaspect);
        else
            renderSetAspect(mulscale16(vr, viewingrange), yxaspect);

        if (videoGetRenderMode() >= REND_POLYMOST)
        {
            if (ud.screen_tilting)
            {
                renderSetRollAngle(fix16_to_float(pPlayer->q16rotscrnang));
                pPlayer->oq16rotscrnang = pPlayer->q16rotscrnang;
            }
            else
            {
                renderSetRollAngle(0);
            }
        }

        if (pPlayer->newowner < 0)
        {
            vec3_t const camVect = { pPlayer->opos.x + mulscale16(pPlayer->pos.x - pPlayer->opos.x, smoothRatio),
                                     pPlayer->opos.y + mulscale16(pPlayer->pos.y - pPlayer->opos.y, smoothRatio),
                                     pPlayer->opos.z + mulscale16(pPlayer->pos.z - pPlayer->opos.z, smoothRatio) };

            CAMERA(pos)          = camVect;

            if (pPlayer->wackedbyactor >= 0)
            {
                CAMERA(q16ang)   = pPlayer->oq16ang
                                 + mulscale16(((pPlayer->q16ang + F16(1024) - pPlayer->oq16ang) & 0x7FFFFFF) - F16(1024), smoothRatio)
                                 + pPlayer->q16look_ang;
                CAMERA(q16horiz) = pPlayer->oq16horiz + pPlayer->oq16horizoff
                                 + mulscale16((pPlayer->q16horiz + pPlayer->q16horizoff - pPlayer->oq16horiz - pPlayer->oq16horizoff), smoothRatio);
            }
            else
            {
                CAMERA(q16ang)   = pPlayer->q16ang + pPlayer->q16look_ang;
                CAMERA(q16horiz) = pPlayer->q16horiz + pPlayer->q16horizoff;
            }

            if (cl_viewbob)
            {
                int zAdd = (pPlayer->opyoff + mulscale16(pPlayer->pyoff-pPlayer->opyoff, smoothRatio));

                if (pPlayer->over_shoulder_on)
                    zAdd >>= 3;

                CAMERA(pos.z) += zAdd;
            }

            if (pPlayer->over_shoulder_on)
            {
                CAMERA(pos.z) -= 3072;

                if (G_DoThirdPerson(pPlayer, &CAMERA(pos), &CAMERA(sect), fix16_to_int(CAMERA(q16ang)), fix16_to_int(CAMERA(q16horiz))) < 0)
                {
                    CAMERA(pos.z) += 3072;
                    G_DoThirdPerson(pPlayer, &CAMERA(pos), &CAMERA(sect), fix16_to_int(CAMERA(q16ang)), fix16_to_int(CAMERA(q16horiz)));
                }
            }
        }
        else
        {
            vec3_t const camVect = G_GetCameraPosition(pPlayer->newowner, smoothRatio);

            // looking through viewscreen
            CAMERA(pos)      = camVect;
            CAMERA(q16ang)   = pPlayer->q16ang + pPlayer->q16look_ang;
            CAMERA(q16horiz) = fix16_from_int(100 + sprite[pPlayer->newowner].shade);
            CAMERA(sect)     = sprite[pPlayer->newowner].sectnum;
        }

        ceilZ  = actor[pPlayer->i].ceilingz;
        floorZ = actor[pPlayer->i].floorz;

        if (g_earthquakeTime > 0 && pPlayer->on_ground == 1)
        {
            CAMERA(pos.z) += 256 - (((g_earthquakeTime)&1) << 9);
            CAMERA(q16ang)   += fix16_from_int((2 - ((g_earthquakeTime)&2)) << 2);
        }

        if (sprite[pPlayer->i].pal == 1)
            CAMERA(pos.z) -= (18<<8);

        if (pPlayer->newowner < 0 && pPlayer->spritebridge == 0)
        {
            // NOTE: when shrunk, p->pos.z can be below the floor.  This puts the
            // camera into the sector again then.

            if (CAMERA(pos.z) < (pPlayer->truecz + ZOFFSET6))
                CAMERA(pos.z) = ceilZ + ZOFFSET6;
            else if (CAMERA(pos.z) > (pPlayer->truefz - ZOFFSET6))
                CAMERA(pos.z) = floorZ - ZOFFSET6;
        }

        while (CAMERA(sect) >= 0)  // if, really
        {
            getzsofslope(CAMERA(sect),CAMERA(pos.x),CAMERA(pos.y),&ceilZ,&floorZ);
#ifdef YAX_ENABLE
            if (yax_getbunch(CAMERA(sect), YAX_CEILING) >= 0)
            {
                if (CAMERA(pos.z) < ceilZ)
                {
                    updatesectorz(CAMERA(pos.x), CAMERA(pos.y), CAMERA(pos.z), &CAMERA(sect));
                    break;  // since CAMERA(sect) might have been updated to -1
                    // NOTE: fist discovered in WGR2 SVN r134, til' death level 1
                    //  (Lochwood Hollow).  A problem REMAINS with Polymost, maybe classic!
                }
            }
            else
#endif
                if (CAMERA(pos.z) < ceilZ+ZOFFSET6)
                    CAMERA(pos.z) = ceilZ+ZOFFSET6;

#ifdef YAX_ENABLE
            if (yax_getbunch(CAMERA(sect), YAX_FLOOR) >= 0)
            {
                if (CAMERA(pos.z) > floorZ)
                    updatesectorz(CAMERA(pos.x), CAMERA(pos.y), CAMERA(pos.z), &CAMERA(sect));
            }
            else
#endif
                if (CAMERA(pos.z) > floorZ-ZOFFSET6)
                    CAMERA(pos.z) = floorZ-ZOFFSET6;

            break;
        }

        // NOTE: might be rendering off-screen here, so CON commands that draw stuff
        //  like showview must cope with that situation or bail out!
        int const noDraw = VM_OnEventWithReturn(EVENT_DISPLAYROOMS, pPlayer->i, playerNum, 0);

        CAMERA(q16horiz) = fix16_clamp(CAMERA(q16horiz), F16(HORIZ_MIN), F16(HORIZ_MAX));

        if (noDraw != 1)  // event return values other than 0 and 1 are reserved
        {
#ifdef DEBUGGINGAIDS
            if (EDUKE32_PREDICT_FALSE(noDraw != 0))
                Printf(TEXTCOLOR_RED "ERROR: EVENT_DISPLAYROOMS return value must be 0 or 1, "
                           "other values are reserved.\n");
#endif
            renderBeginScene();

            G_HandleMirror(CAMERA(pos.x), CAMERA(pos.y), CAMERA(pos.z), CAMERA(q16ang), CAMERA(q16horiz), smoothRatio);
            G_ClearGotMirror();
#ifdef LEGACY_ROR
            G_SE40(smoothRatio);
#endif
#ifdef POLYMER
            if (videoGetRenderMode() == REND_POLYMER)
                polymer_setanimatesprites(G_DoSpriteAnimations, CAMERA(pos.x),CAMERA(pos.y),CAMERA(pos.z),fix16_to_int(CAMERA(q16ang)),smoothRatio);
#endif
            // for G_PrintCoords
            dr_viewingrange = viewingrange;
            dr_yxaspect = yxaspect;
#ifdef DEBUG_MIRRORS_ONLY
            gotpic[MIRROR>>3] |= pow2char[MIRROR&7];
#else
            yax_preparedrawrooms();
            renderDrawRoomsQ16(CAMERA(pos.x),CAMERA(pos.y),CAMERA(pos.z),CAMERA(q16ang),CAMERA(q16horiz),CAMERA(sect));
            yax_drawrooms(G_DoSpriteAnimations, CAMERA(sect), 0, smoothRatio);
#ifdef LEGACY_ROR
            if ((unsigned)ror_sprite < MAXSPRITES && drawing_ror == 1)  // viewing from bottom
                G_OROR_DupeSprites(&sprite[ror_sprite]);
#endif
            G_DoSpriteAnimations(CAMERA(pos.x),CAMERA(pos.y),CAMERA(pos.z),fix16_to_int(CAMERA(q16ang)),smoothRatio);
#ifdef LEGACY_ROR
            drawing_ror = 0;
#endif
            renderDrawMasks();
#endif
            renderFinishScene();
        }
    }

    G_RestoreInterpolations();

    {
        // Totalclock count of last step of p->visibility converging towards
        // ud.const_visibility.
        static int32_t lastvist;
        const int32_t visdif = ud.const_visibility-pPlayer->visibility;

        // Check if totalclock was cleared (e.g. restarted game).
        if (totalclock < lastvist)
            lastvist = 0;

        // Every 2nd totalclock increment (each 1/60th second), ...
        while (totalclock >= lastvist+2)
        {
            // ... approximately three-quarter the difference between
            // p->visibility and ud.const_visibility.
            const int32_t visinc = visdif>>2;

            if (klabs(visinc) == 0)
            {
                pPlayer->visibility = ud.const_visibility;
                break;
            }

            pPlayer->visibility += visinc;
            lastvist = (int32_t) totalclock;
        }
    }

    if (r_usenewaspect)
    {
        newaspect_enable = 0;
        renderSetAspect(viewingRange, tabledivide32_noinline(65536 * ydim * 8, xdim * 5));
    }

    VM_OnEvent(EVENT_DISPLAYROOMSEND, g_player[screenpeek].ps->i, screenpeek);
}


bool GameInterface::GenerateSavePic()
{
    G_DrawRooms(myconnectindex, 65536);
    return true;
}


void G_DumpDebugInfo(void)
{
    static char const s_WEAPON[] = "WEAPON";
    int32_t i,j,x;

    VM_ScriptInfo(insptr, 64);
    buildprint("\nCurrent gamevar values:\n");

    for (i=0; i<MAX_WEAPONS; i++)
    {
        for (j=0; j<numplayers; j++)
        {
            buildprint("Player ", j, "\n\n");
            buildprint(s_WEAPON, i, "_CLIP ", PWEAPON(j, i, Clip), "\n");
            buildprint(s_WEAPON, i, "_RELOAD ", PWEAPON(j, i, Reload), "\n");
            buildprint(s_WEAPON, i, "_FIREDELAY ", PWEAPON(j, i, FireDelay), "\n");
            buildprint(s_WEAPON, i, "_TOTALTIME ", PWEAPON(j, i, TotalTime), "\n");
            buildprint(s_WEAPON, i, "_HOLDDELAY ", PWEAPON(j, i, HoldDelay), "\n");
            buildprint(s_WEAPON, i, "_FLAGS ", PWEAPON(j, i, Flags), "\n");
            buildprint(s_WEAPON, i, "_SHOOTS ", PWEAPON(j, i, Shoots), "\n");
            buildprint(s_WEAPON, i, "_SPAWNTIME ", PWEAPON(j, i, SpawnTime), "\n");
            buildprint(s_WEAPON, i, "_SPAWN ", PWEAPON(j, i, Spawn), "\n");
            buildprint(s_WEAPON, i, "_SHOTSPERBURST ", PWEAPON(j, i, ShotsPerBurst), "\n");
            buildprint(s_WEAPON, i, "_WORKSLIKE ", PWEAPON(j, i, WorksLike), "\n");
            buildprint(s_WEAPON, i, "_INITIALSOUND ", PWEAPON(j, i, InitialSound), "\n");
            buildprint(s_WEAPON, i, "_FIRESOUND ", PWEAPON(j, i, FireSound), "\n");
            buildprint(s_WEAPON, i, "_SOUND2TIME ", PWEAPON(j, i, Sound2Time), "\n");
            buildprint(s_WEAPON, i, "_SOUND2SOUND ", PWEAPON(j, i, Sound2Sound), "\n");
            buildprint(s_WEAPON, i, "_RELOADSOUND1 ", PWEAPON(j, i, ReloadSound1), "\n");
            buildprint(s_WEAPON, i, "_RELOADSOUND2 ", PWEAPON(j, i, ReloadSound2), "\n");
            buildprint(s_WEAPON, i, "_SELECTSOUND ", PWEAPON(j, i, SelectSound), "\n");
            buildprint(s_WEAPON, i, "_FLASHCOLOR ", PWEAPON(j, i, FlashColor), "\n");
        }
        buildprint("\n");
    }

    for (x=0; x<MAXSTATUS; x++)
    {
        j = headspritestat[x];
        while (j >= 0)
        {
            buildprint("Sprite ", j, " (", TrackerCast(sprite[j].x), ",", TrackerCast(sprite[j].y), ",", TrackerCast(sprite[j].z),
                ") (picnum: ", TrackerCast(sprite[j].picnum), ")\n");
            for (i=0; i<g_gameVarCount; i++)
            {
                if (aGameVars[i].flags & (GAMEVAR_PERACTOR))
                {
                    if (aGameVars[i].pValues[j] != aGameVars[i].defaultValue)
                    {
                        buildprint("gamevar ", aGameVars[i].szLabel, " ", aGameVars[i].pValues[j], " GAMEVAR_PERACTOR");
                        if (aGameVars[i].flags != GAMEVAR_PERACTOR)
                        {
                            buildprint(" // ");
                            if (aGameVars[i].flags & (GAMEVAR_SYSTEM))
                            {
                                buildprint(" (system)");
                            }
                        }
                        buildprint("\n");
                    }
                }
            }
            buildprint("\n");
            j = nextspritestat[j];
        }
    }
    Gv_DumpValues();
}

// if <set_movflag_uncond> is true, set the moveflag unconditionally,
// else only if it equals 0.
static int32_t G_InitActor(int32_t i, int32_t tilenum, int32_t set_movflag_uncond)
{
    if (g_tile[tilenum].execPtr)
    {
        SH(i) = *(g_tile[tilenum].execPtr);
        AC_ACTION_ID(actor[i].t_data) = *(g_tile[tilenum].execPtr+1);
        AC_MOVE_ID(actor[i].t_data) = *(g_tile[tilenum].execPtr+2);

        if (set_movflag_uncond || SHT(i) == 0)  // AC_MOVFLAGS
            SHT(i) = *(g_tile[tilenum].execPtr+3);

        return 1;
    }
    return 0;
}

int32_t A_InsertSprite(int16_t whatsect,int32_t s_x,int32_t s_y,int32_t s_z,int16_t s_pn,int8_t s_s,
                       uint8_t s_xr,uint8_t s_yr,int16_t s_a,int16_t s_ve,int16_t s_zv,int16_t s_ow,int16_t s_ss)
{


    int32_t newSprite;

#ifdef NETCODE_DISABLE
    newSprite = insertsprite(whatsect, s_ss);
#else
    newSprite = Net_InsertSprite(whatsect, s_ss);

#endif

    if (EDUKE32_PREDICT_FALSE((unsigned)newSprite >= MAXSPRITES))
    {
        G_DumpDebugInfo();
        Printf("Failed spawning pic %d spr from pic %d spr %d at x:%d,y:%d,z:%d,sect:%d\n",
                          s_pn,s_ow < 0 ? -1 : TrackerCast(sprite[s_ow].picnum),s_ow,s_x,s_y,s_z,whatsect);
        G_GameExit("Too many sprites spawned.");
    }

#ifdef DEBUGGINGAIDS
    g_spriteStat.numins++;
#endif

    sprite[newSprite] = { s_x, s_y, s_z, 0, s_pn, s_s, 0, 0, 0, s_xr, s_yr, 0, 0, whatsect, s_ss, s_a, s_ow, s_ve, 0, s_zv, 0, 0, 0 };

    auto &a = actor[newSprite];
    a = {};
    a.bpos = { s_x, s_y, s_z };

    if ((unsigned)s_ow < MAXSPRITES)
    {
        a.picnum   = sprite[s_ow].picnum;
        a.floorz   = actor[s_ow].floorz;
        a.ceilingz = actor[s_ow].ceilingz;
    }

    a.stayput = -1;
    a.extra   = -1;
#ifdef POLYMER
    a.lightId = -1;
#endif
    a.owner = s_ow;

    G_InitActor(newSprite, s_pn, 1);

    spriteext[newSprite]    = {};
    spritesmooth[newSprite] = {};

#if defined LUNATIC
    if (!g_noResetVars)
#endif
        A_ResetVars(newSprite);
#if defined LUNATIC
    g_noResetVars = 0;
#endif

    if (VM_HaveEvent(EVENT_EGS))
    {
        int32_t p, pl = A_FindPlayer(&sprite[newSprite], &p);

        block_deletesprite++;
        VM_ExecuteEvent(EVENT_EGS, newSprite, pl, p);
        block_deletesprite--;
    }

    return newSprite;
}

#ifdef YAX_ENABLE
void Yax_SetBunchZs(int32_t sectnum, int32_t cf, int32_t daz)
{
    int32_t i, bunchnum = yax_getbunch(sectnum, cf);

    if (bunchnum < 0 || bunchnum >= numyaxbunches)
        return;

    for (SECTORS_OF_BUNCH(bunchnum, YAX_CEILING, i))
        SECTORFLD(i,z, YAX_CEILING) = daz;
    for (SECTORS_OF_BUNCH(bunchnum, YAX_FLOOR, i))
        SECTORFLD(i,z, YAX_FLOOR) = daz;
}

static void Yax_SetBunchInterpolation(int32_t sectnum, int32_t cf)
{
    int32_t i, bunchnum = yax_getbunch(sectnum, cf);

    if (bunchnum < 0 || bunchnum >= numyaxbunches)
        return;

    for (SECTORS_OF_BUNCH(bunchnum, YAX_CEILING, i))
        G_SetInterpolation(&sector[i].ceilingz);
    for (SECTORS_OF_BUNCH(bunchnum, YAX_FLOOR, i))
        G_SetInterpolation(&sector[i].floorz);
}
#else
# define Yax_SetBunchInterpolation(sectnum, cf)
#endif

// A_Spawn has two forms with arguments having different meaning:
//
// 1. spriteNum>=0: Spawn from parent sprite <spriteNum> with picnum <tileNum>
// 2. spriteNum<0: Spawn from already *existing* sprite <tileNum>
int A_Spawn(int spriteNum, int tileNum)
{
    int         newSprite;
    spritetype *pSprite;
    actor_t *   pActor;
    int         sectNum;

    if (spriteNum >= 0)
    {
        // spawn from parent sprite <j>
        newSprite = A_InsertSprite(sprite[spriteNum].sectnum,sprite[spriteNum].x,sprite[spriteNum].y,sprite[spriteNum].z,
                           tileNum,0,0,0,0,0,0,spriteNum,0);
        actor[newSprite].picnum = sprite[spriteNum].picnum;
    }
    else
    {
        // spawn from already existing sprite <pn>
        newSprite = tileNum;
        auto &s = sprite[newSprite];
        auto &a = actor[newSprite];

        a = { };
        a.bpos = { s.x, s.y, s.z };

        a.picnum = s.picnum;

        if (s.picnum == SECTOREFFECTOR && s.lotag == 50)
            a.picnum = s.owner;

        if (s.picnum == LOCATORS && s.owner != -1)
            a.owner = s.owner;
        else
            s.owner = a.owner = newSprite;

        a.floorz   = sector[s.sectnum].floorz;
        a.ceilingz = sector[s.sectnum].ceilingz;
        a.stayput  = a.extra = -1;

#ifdef POLYMER
        a.lightId = -1;
#endif

        if ((s.cstat & 48)
#ifndef EDUKE32_STANDALONE
            && s.picnum != SPEAKER && s.picnum != LETTER && s.picnum != DUCK && s.picnum != TARGET && s.picnum != TRIPBOMB
#endif
            && s.picnum != VIEWSCREEN && s.picnum != VIEWSCREEN2 && (!(s.picnum >= CRACK1 && s.picnum <= CRACK4)))
        {
            if (s.shade == 127)
                goto SPAWN_END;

#ifndef EDUKE32_STANDALONE
            if (A_CheckSwitchTile(newSprite) && (s.cstat & 16))
            {
                if (s.pal && s.picnum != ACCESSSWITCH && s.picnum != ACCESSSWITCH2)
                {
                    if (((!g_netServer && ud.multimode < 2)) || ((g_netServer || ud.multimode > 1) && !GTFLAGS(GAMETYPE_DMSWITCHES)))
                    {
                        s.xrepeat = s.yrepeat = 0;
                        s.lotag = s.hitag = 0;
                        s.cstat = 32768;
                        goto SPAWN_END;
                    }
                }

                s.cstat |= 257;

                if (s.pal && s.picnum != ACCESSSWITCH && s.picnum != ACCESSSWITCH2)
                    s.pal = 0;

                goto SPAWN_END;
            }
#endif

            if (s.hitag)
            {
                changespritestat(newSprite, STAT_FALLER);
                s.cstat |= 257;
                s.extra = g_impactDamage;
                goto SPAWN_END;
            }
        }

        if (s.cstat & 1)
            s.cstat |= 256;

        if (!G_InitActor(newSprite, s.picnum, 0))
            T2(newSprite) = T5(newSprite) = 0;  // AC_MOVE_ID, AC_ACTION_ID
        else
        {
            A_GetZLimits(newSprite);
            actor[newSprite].bpos = sprite[newSprite].pos;
        }
    }

    pSprite = &sprite[newSprite];
    pActor  = &actor[newSprite];
    sectNum = pSprite->sectnum;

    //some special cases that can't be handled through the dynamictostatic system.

    if (pSprite->picnum >= CAMERA1 && pSprite->picnum <= CAMERA1 + 4)
        pSprite->picnum = CAMERA1;
#ifndef EDUKE32_STANDALONE
    else if (pSprite->picnum >= BOLT1 && pSprite->picnum <= BOLT1 + 3)
        pSprite->picnum = BOLT1;
    else if (pSprite->picnum >= SIDEBOLT1 && pSprite->picnum <= SIDEBOLT1 + 3)
        pSprite->picnum = SIDEBOLT1;
#endif
        switch (DYNAMICTILEMAP(pSprite->picnum))
        {
        case FOF__STATIC:
            pSprite->xrepeat = pSprite->yrepeat = 0;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;
        case CAMERA1__STATIC:
            pSprite->extra = 1;
            pSprite->cstat &= 32768;

            if (g_damageCameras)
                pSprite->cstat |= 257;

            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
            }
            else
            {
                pSprite->pal = 0;
                changespritestat(newSprite, STAT_ACTOR);
            }
            goto SPAWN_END;
#ifndef EDUKE32_STANDALONE
        case CAMERAPOLE__STATIC:
            pSprite->extra = 1;
            pSprite->cstat &= 32768;

            if (g_damageCameras)
                pSprite->cstat |= 257;
            fallthrough__;
        case GENERICPOLE__STATIC:
            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
            }
            else
                pSprite->pal = 0;
            goto SPAWN_END;

        case BOLT1__STATIC:
        case SIDEBOLT1__STATIC:
            T1(newSprite) = pSprite->xrepeat;
            T2(newSprite) = pSprite->yrepeat;
            pSprite->yvel = 0;

            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case WATERSPLASH2__STATIC:
            if (spriteNum >= 0)
            {
                setsprite(newSprite, &sprite[spriteNum].pos);
                pSprite->xrepeat = pSprite->yrepeat = 8+(krand()&7);
            }
            else pSprite->xrepeat = pSprite->yrepeat = 16+(krand()&15);

            pSprite->shade = -16;
            pSprite->cstat |= 128;

            if (spriteNum >= 0)
            {
                if (sector[sprite[spriteNum].sectnum].lotag == ST_2_UNDERWATER)
                {
                    pSprite->z = getceilzofslope(sectNum, pSprite->x, pSprite->y) + (16 << 8);
                    pSprite->cstat |= 8;
                }
                else if (sector[sprite[spriteNum].sectnum].lotag == ST_1_ABOVE_WATER)
                    pSprite->z = getflorzofslope(sectNum, pSprite->x, pSprite->y);
            }

            if (sector[sectNum].floorpicnum == FLOORSLIME || sector[sectNum].ceilingpicnum == FLOORSLIME)
                pSprite->pal = 7;
            fallthrough__;
        case DOMELITE__STATIC:
            if (pSprite->picnum == DOMELITE)
                pSprite->cstat |= 257;
            fallthrough__;
        case NEON1__STATIC:
        case NEON2__STATIC:
        case NEON3__STATIC:
        case NEON4__STATIC:
        case NEON5__STATIC:
        case NEON6__STATIC:
            if (pSprite->picnum != WATERSPLASH2)
                pSprite->cstat |= 257;
            fallthrough__;
        case NUKEBUTTON__STATIC:
        case JIBS1__STATIC:
        case JIBS2__STATIC:
        case JIBS3__STATIC:
        case JIBS4__STATIC:
        case JIBS5__STATIC:
        case JIBS6__STATIC:
        case HEADJIB1__STATIC:
        case ARMJIB1__STATIC:
        case LEGJIB1__STATIC:
        case LIZMANHEAD1__STATIC:
        case LIZMANARM1__STATIC:
        case LIZMANLEG1__STATIC:
        case DUKETORSO__STATIC:
        case DUKEGUN__STATIC:
        case DUKELEG__STATIC:
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;
        case TONGUE__STATIC:
            if (spriteNum >= 0)
                pSprite->ang = sprite[spriteNum].ang;
            pSprite->z -= 38<<8;
            pSprite->zvel = 256-(krand()&511);
            pSprite->xvel = 64-(krand()&127);
            changespritestat(newSprite, STAT_PROJECTILE);
            goto SPAWN_END;
        case NATURALLIGHTNING__STATIC:
            pSprite->cstat &= ~257;
            pSprite->cstat |= 32768;
            goto SPAWN_END;
        case TRANSPORTERSTAR__STATIC:
        case TRANSPORTERBEAM__STATIC:
            if (spriteNum == -1)
                goto SPAWN_END;
            if (pSprite->picnum == TRANSPORTERBEAM)
            {
                pSprite->xrepeat = 31;
                pSprite->yrepeat = 1;
                pSprite->z = sector[sprite[spriteNum].sectnum].floorz-PHEIGHT;
            }
            else
            {
                if (sprite[spriteNum].statnum == STAT_PROJECTILE)
                    pSprite->xrepeat = pSprite->yrepeat = 8;
                else
                {
                    pSprite->xrepeat = 48;
                    pSprite->yrepeat = 64;
                    if (sprite[spriteNum].statnum == STAT_PLAYER || A_CheckEnemySprite(&sprite[spriteNum]))
                        pSprite->z -= ZOFFSET5;
                }
            }

            pSprite->shade = -127;
            pSprite->cstat = 128|2;
            pSprite->ang = sprite[spriteNum].ang;

            pSprite->xvel = 128;
            changespritestat(newSprite, STAT_MISC);
            A_SetSprite(newSprite,CLIPMASK0);
            setsprite(newSprite,&pSprite->pos);
            goto SPAWN_END;
        case FEMMAG1__STATIC:
        case FEMMAG2__STATIC:
            pSprite->cstat &= ~257;
            changespritestat(newSprite, STAT_DEFAULT);
            goto SPAWN_END;
        case DUKETAG__STATIC:
        case SIGN1__STATIC:
        case SIGN2__STATIC:
            if ((!g_netServer && ud.multimode < 2) && pSprite->pal)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
            }
            else pSprite->pal = 0;
            goto SPAWN_END;

        case MASKWALL1__STATIC:
        case MASKWALL2__STATIC:
        case MASKWALL3__STATIC:
        case MASKWALL4__STATIC:
        case MASKWALL5__STATIC:
        case MASKWALL6__STATIC:
        case MASKWALL7__STATIC:
        case MASKWALL8__STATIC:
        case MASKWALL9__STATIC:
        case MASKWALL10__STATIC:
        case MASKWALL11__STATIC:
        case MASKWALL12__STATIC:
        case MASKWALL13__STATIC:
        case MASKWALL14__STATIC:
        case MASKWALL15__STATIC:
        {
            int const j    = pSprite->cstat & SPAWN_PROTECT_CSTAT_MASK;
            pSprite->cstat = j | CSTAT_SPRITE_BLOCK;
            changespritestat(newSprite, STAT_DEFAULT);
            goto SPAWN_END;
        }

        case PODFEM1__STATIC:
            pSprite->extra <<= 1;
            fallthrough__;
        case FEM1__STATIC:
        case FEM2__STATIC:
        case FEM3__STATIC:
        case FEM4__STATIC:
        case FEM5__STATIC:
        case FEM6__STATIC:
        case FEM7__STATIC:
        case FEM8__STATIC:
        case FEM9__STATIC:
        case FEM10__STATIC:
        case NAKED1__STATIC:
        case STATUE__STATIC:
        case TOUGHGAL__STATIC:
            pSprite->yvel  = pSprite->hitag;
            pSprite->hitag = -1;
            fallthrough__;
        case BLOODYPOLE__STATIC:
            pSprite->cstat   |= 257;
            pSprite->clipdist = 32;
            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case QUEBALL__STATIC:
        case STRIPEBALL__STATIC:
            pSprite->cstat    = 256;
            pSprite->clipdist = 8;
            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case DUKELYINGDEAD__STATIC:
            if (spriteNum >= 0 && sprite[spriteNum].picnum == APLAYER)
            {
                pSprite->xrepeat = sprite[spriteNum].xrepeat;
                pSprite->yrepeat = sprite[spriteNum].yrepeat;
                pSprite->shade   = sprite[spriteNum].shade;
                pSprite->pal     = g_player[P_Get(spriteNum)].ps->palookup;
            }
            fallthrough__;
        case DUKECAR__STATIC:
        case HELECOPT__STATIC:
            //                if(sp->picnum == HELECOPT || sp->picnum == DUKECAR) sp->xvel = 1024;
            pSprite->cstat = 0;
            pSprite->extra = 1;
            pSprite->xvel  = 292;
            pSprite->zvel  = 360;
            fallthrough__;
        case BLIMP__STATIC:
            pSprite->cstat   |= 257;
            pSprite->clipdist = 128;
            changespritestat(newSprite, STAT_ACTOR);
            goto SPAWN_END;

        case RESPAWNMARKERRED__STATIC:
            pSprite->xrepeat = pSprite->yrepeat = 24;
            if (spriteNum >= 0)
                pSprite->z = actor[spriteNum].floorz;  // -(1<<4);
            changespritestat(newSprite, STAT_ACTOR);
            goto SPAWN_END;

        case MIKE__STATIC:
            pSprite->yvel  = pSprite->hitag;
            pSprite->hitag = 0;
            changespritestat(newSprite, STAT_ACTOR);
            goto SPAWN_END;
        case WEATHERWARN__STATIC:
            changespritestat(newSprite, STAT_ACTOR);
            goto SPAWN_END;

        case SPOTLITE__STATIC:
            T1(newSprite) = pSprite->x;
            T2(newSprite) = pSprite->y;
            goto SPAWN_END;
        case BULLETHOLE__STATIC:
            pSprite->xrepeat = 3;
            pSprite->yrepeat = 3;
            pSprite->cstat   = 16 + (krand() & 12);

            A_AddToDeleteQueue(newSprite);
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case MONEY__STATIC:
        case MAIL__STATIC:
        case PAPER__STATIC:
            pActor->t_data[0] = krand() & 2047;

            pSprite->cstat   = krand() & 12;
            pSprite->xrepeat = 8;
            pSprite->yrepeat = 8;
            pSprite->ang     = krand() & 2047;

            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case SHELL__STATIC: //From the player
        case SHOTGUNSHELL__STATIC:
            if (spriteNum >= 0)
            {
                int shellAng;

                if (sprite[spriteNum].picnum == APLAYER)
                {
                    int const  playerNum = P_Get(spriteNum);
                    auto const pPlayer   = g_player[playerNum].ps;

                    shellAng = fix16_to_int(pPlayer->q16ang) - (krand() & 63) + 8;  // Fine tune

                    T1(newSprite) = krand() & 1;

                    pSprite->z = (3 << 8) + pPlayer->pyoff + pPlayer->pos.z - (fix16_to_int((pPlayer->q16horizoff + pPlayer->q16horiz - F16(100))) << 4);

                    if (pSprite->picnum == SHOTGUNSHELL)
                        pSprite->z += (3 << 8);

                    pSprite->zvel = -(krand() & 255);
                }
                else
                {
                    shellAng          = pSprite->ang;
                    pSprite->z = sprite[spriteNum].z - PHEIGHT + (3 << 8);
                }

                pSprite->x     = sprite[spriteNum].x + (sintable[(shellAng + 512) & 2047] >> 7);
                pSprite->y     = sprite[spriteNum].y + (sintable[shellAng & 2047] >> 7);
                pSprite->shade = -8;

                if (pSprite->yvel == 1 || NAM_WW2GI)
                {
                    pSprite->ang  = shellAng + 512;
                    pSprite->xvel = 30;
                }
                else
                {
                    pSprite->ang  = shellAng - 512;
                    pSprite->xvel = 20;
                }

                pSprite->xrepeat = pSprite->yrepeat = 4;

                changespritestat(newSprite, STAT_MISC);
            }
            goto SPAWN_END;

        case WATERBUBBLE__STATIC:
            if (spriteNum >= 0)
            {
                if (sprite[spriteNum].picnum == APLAYER)
                    pSprite->z -= (16 << 8);

                pSprite->ang = sprite[spriteNum].ang;
            }

            pSprite->xrepeat = pSprite->yrepeat = 4;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case CRANE__STATIC:

            pSprite->cstat |= 64|257;

            pSprite->picnum += 2;
            pSprite->z = sector[sectNum].ceilingz+(48<<8);
            T5(newSprite) = tempwallptr;

            g_origins[tempwallptr] = pSprite->pos.vec2;
            g_origins[tempwallptr+2].x = pSprite->z;


            if (headspritestat[STAT_DEFAULT] != -1)
            {
                int findSprite = headspritestat[STAT_DEFAULT];

                do
                {
                    if (sprite[findSprite].picnum == CRANEPOLE && pSprite->hitag == (sprite[findSprite].hitag))
                    {
                        g_origins[tempwallptr + 2].y = findSprite;

                        T2(newSprite) = sprite[findSprite].sectnum;

                        sprite[findSprite].xrepeat = 48;
                        sprite[findSprite].yrepeat = 128;

                        g_origins[tempwallptr + 1] = sprite[findSprite].pos.vec2;
                        sprite[findSprite].pos     = pSprite->pos;
                        sprite[findSprite].shade   = pSprite->shade;

                        setsprite(findSprite, &sprite[findSprite].pos);
                        break;
                    }
                    findSprite = nextspritestat[findSprite];
                } while (findSprite >= 0);
            }

            tempwallptr += 3;
            pSprite->owner = -1;
            pSprite->extra = 8;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case TRASH__STATIC:
            pSprite->ang = krand()&2047;
            pSprite->xrepeat = pSprite->yrepeat = 24;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case WATERDRIP__STATIC:
            if (spriteNum >= 0 && (sprite[spriteNum].statnum == STAT_PLAYER || sprite[spriteNum].statnum == STAT_ACTOR))
            {
                if (sprite[spriteNum].pal != 1)
                {
                    pSprite->pal = 2;
                    pSprite->z -= (18<<8);
                }
                else pSprite->z -= (13<<8);

                pSprite->shade = 32;
                pSprite->ang   = getangle(g_player[0].ps->pos.x - pSprite->x, g_player[0].ps->pos.y - pSprite->y);
                pSprite->xvel  = 48 - (krand() & 31);

                A_SetSprite(newSprite, CLIPMASK0);
            }
            else if (spriteNum == -1)
            {
                pSprite->z += ZOFFSET6;
                T1(newSprite) = pSprite->z;
                T2(newSprite) = krand()&127;
            }
            fallthrough__;
        case WATERDRIPSPLASH__STATIC:
            pSprite->xrepeat = pSprite->yrepeat = 24;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case PLUG__STATIC:
            pSprite->lotag = 9999;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;
        case TARGET__STATIC:
        case DUCK__STATIC:
        case LETTER__STATIC:
            pSprite->extra = 1;
            pSprite->cstat |= 257;
            changespritestat(newSprite, STAT_ACTOR);
            goto SPAWN_END;

        case BOSS2STAYPUT__STATIC:
        case BOSS3STAYPUT__STATIC:
        case BOSS5STAYPUT__STATIC:
            if (!WORLDTOUR)
                break;
            fallthrough__;
        case OCTABRAINSTAYPUT__STATIC:
        case LIZTROOPSTAYPUT__STATIC:
        case PIGCOPSTAYPUT__STATIC:
        case LIZMANSTAYPUT__STATIC:
        case BOSS1STAYPUT__STATIC:
        case PIGCOPDIVE__STATIC:
        case COMMANDERSTAYPUT__STATIC:
        case BOSS4STAYPUT__STATIC:
            pActor->stayput = pSprite->sectnum;
            fallthrough__;
        case GREENSLIME__STATIC:
            if (pSprite->picnum == GREENSLIME)
                pSprite->extra = 1;
            fallthrough__;
        case BOSS5__STATIC:
        case FIREFLY__STATIC:
            if (!WORLDTOUR && (pSprite->picnum == BOSS5 || pSprite->picnum == FIREFLY))
                break;
            fallthrough__;
        case BOSS1__STATIC:
        case BOSS2__STATIC:
        case BOSS3__STATIC:
        case BOSS4__STATIC:
        case ROTATEGUN__STATIC:
        case DRONE__STATIC:
        case LIZTROOPONTOILET__STATIC:
        case LIZTROOPJUSTSIT__STATIC:
        case LIZTROOPSHOOT__STATIC:
        case LIZTROOPJETPACK__STATIC:
        case LIZTROOPDUCKING__STATIC:
        case LIZTROOPRUNNING__STATIC:
        case LIZTROOP__STATIC:
        case OCTABRAIN__STATIC:
        case COMMANDER__STATIC:
        case PIGCOP__STATIC:
        case LIZMAN__STATIC:
        case LIZMANSPITTING__STATIC:
        case LIZMANFEEDING__STATIC:
        case LIZMANJUMP__STATIC:
        case ORGANTIC__STATIC:
        case RAT__STATIC:
        case SHARK__STATIC:

            if (pSprite->pal == 0)
            {
                switch (DYNAMICTILEMAP(pSprite->picnum))
                {
                case LIZTROOPONTOILET__STATIC:
                case LIZTROOPSHOOT__STATIC:
                case LIZTROOPJETPACK__STATIC:
                case LIZTROOPDUCKING__STATIC:
                case LIZTROOPRUNNING__STATIC:
                case LIZTROOPSTAYPUT__STATIC:
                case LIZTROOPJUSTSIT__STATIC:
                case LIZTROOP__STATIC: pSprite->pal = 22; break;
                }
            }
            else
            {
                if (!PLUTOPAK)
                    pSprite->extra <<= 1;
            }

            if (pSprite->picnum == BOSS4STAYPUT || pSprite->picnum == BOSS1 || pSprite->picnum == BOSS2 ||
                pSprite->picnum == BOSS1STAYPUT || pSprite->picnum == BOSS3 || pSprite->picnum == BOSS4 ||
                (WORLDTOUR && (pSprite->picnum == BOSS2STAYPUT || pSprite->picnum == BOSS3STAYPUT ||
                pSprite->picnum == BOSS5STAYPUT || pSprite->picnum == BOSS5)))
            {
                if (spriteNum >= 0 && sprite[spriteNum].picnum == RESPAWN)
                    pSprite->pal = sprite[spriteNum].pal;

                if (pSprite->pal && (!WORLDTOUR || pSprite->pal != 22))
                {
                    pSprite->clipdist = 80;
                    pSprite->xrepeat  = pSprite->yrepeat = 40;
                }
                else
                {
                    pSprite->xrepeat  = pSprite->yrepeat = 80;
                    pSprite->clipdist = 164;
                }
            }
            else
            {
                if (pSprite->picnum != SHARK)
                {
                    pSprite->xrepeat  = pSprite->yrepeat = 40;
                    pSprite->clipdist = 80;
                }
                else
                {
                    pSprite->xrepeat  = pSprite->yrepeat = 60;
                    pSprite->clipdist = 40;
                }
            }

            // If spawned from parent sprite (as opposed to 'from premap'),
            // ignore skill.
            if (spriteNum >= 0)
                pSprite->lotag = 0;

            if ((pSprite->lotag > ud.player_skill) || ud.monsters_off == 1)
            {
                pSprite->xrepeat=pSprite->yrepeat=0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            else
            {
                A_Fall(newSprite);

                if (pSprite->picnum == RAT)
                {
                    pSprite->ang = krand()&2047;
                    pSprite->xrepeat = pSprite->yrepeat = 48;
                    pSprite->cstat = 0;
                }
                else
                {
                    pSprite->cstat |= 257;

                    if (pSprite->picnum != SHARK)
                        g_player[myconnectindex].ps->max_actors_killed++;
                }

                if (pSprite->picnum == ORGANTIC) pSprite->cstat |= 128;

                if (spriteNum >= 0)
                {
                    pActor->timetosleep = 0;
                    A_PlayAlertSound(newSprite);
                    changespritestat(newSprite, STAT_ACTOR);
                }
                else changespritestat(newSprite, STAT_ZOMBIEACTOR);
            }

            if (pSprite->picnum == ROTATEGUN)
                pSprite->zvel = 0;

            goto SPAWN_END;

        case REACTOR2__STATIC:
        case REACTOR__STATIC:
            pSprite->extra = g_impactDamage;
            pSprite->cstat |= 257;
            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            pSprite->pal   = 0;
            pSprite->shade = -17;

            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case HEAVYHBOMB__STATIC:
            if (spriteNum >= 0)
                pSprite->owner = spriteNum;
            else pSprite->owner = newSprite;

            pSprite->xrepeat = pSprite->yrepeat = 9;
            pSprite->yvel = 4;
            pSprite->cstat |= 257;

            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            pSprite->pal   = 0;
            pSprite->shade = -17;

            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case RECON__STATIC:
            if (pSprite->lotag > ud.player_skill)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            g_player[myconnectindex].ps->max_actors_killed++;
            pActor->t_data[5] = 0;
            if (ud.monsters_off == 1)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            pSprite->extra = 130;
            pSprite->cstat |= 256; // Make it hitable

            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            pSprite->pal   = 0;
            pSprite->shade = -17;

            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case FLAMETHROWERSPRITE__STATIC:
        case FLAMETHROWERAMMO__STATIC:
            if (!WORLDTOUR)
                break;
            fallthrough__;

        case ATOMICHEALTH__STATIC:
        case STEROIDS__STATIC:
        case HEATSENSOR__STATIC:
        case SHIELD__STATIC:
        case AIRTANK__STATIC:
        case TRIPBOMBSPRITE__STATIC:
        case JETPACK__STATIC:
        case HOLODUKE__STATIC:

        case FIRSTGUNSPRITE__STATIC:
        case CHAINGUNSPRITE__STATIC:
        case SHOTGUNSPRITE__STATIC:
        case RPGSPRITE__STATIC:
        case SHRINKERSPRITE__STATIC:
        case FREEZESPRITE__STATIC:
        case DEVISTATORSPRITE__STATIC:

        case SHOTGUNAMMO__STATIC:
        case FREEZEAMMO__STATIC:
        case HBOMBAMMO__STATIC:
        case CRYSTALAMMO__STATIC:
        case GROWAMMO__STATIC:
        case BATTERYAMMO__STATIC:
        case DEVISTATORAMMO__STATIC:
        case RPGAMMO__STATIC:
        case BOOTS__STATIC:
        case AMMO__STATIC:
        case AMMOLOTS__STATIC:
        case COLA__STATIC:
        case FIRSTAID__STATIC:
        case SIXPAK__STATIC:

            if (spriteNum >= 0)
            {
                pSprite->lotag = 0;
                pSprite->z -= ZOFFSET5;
                pSprite->zvel = -1024;
                A_SetSprite(newSprite, CLIPMASK0);
                pSprite->cstat = krand()&4;
            }
            else
            {
                pSprite->owner = newSprite;
                pSprite->cstat = 0;
            }

            if (((!g_netServer && ud.multimode < 2) && pSprite->pal != 0) || (pSprite->lotag > ud.player_skill))
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            pSprite->pal = 0;

            if (pSprite->picnum == ATOMICHEALTH)
                pSprite->cstat |= 128;

            fallthrough__;
        case ACCESSCARD__STATIC:
            if ((g_netServer || ud.multimode > 1) && !GTFLAGS(GAMETYPE_ACCESSCARDSPRITES) && pSprite->picnum == ACCESSCARD)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            else
            {
                if (pSprite->picnum == AMMO)
                    pSprite->xrepeat = pSprite->yrepeat = 16;
                else pSprite->xrepeat = pSprite->yrepeat = 32;
            }

            pSprite->shade = -17;

            if (spriteNum >= 0)
            {
                changespritestat(newSprite, STAT_ACTOR);
            }
            else
            {
                changespritestat(newSprite, STAT_ZOMBIEACTOR);
                A_Fall(newSprite);
            }
            goto SPAWN_END;

        case WATERFOUNTAIN__STATIC:
            SLT(newSprite) = 1;
            fallthrough__;
        case TREE1__STATIC:
        case TREE2__STATIC:
        case TIRE__STATIC:
        case CONE__STATIC:
        case BOX__STATIC:
            pSprite->cstat = 257; // Make it hitable
            sprite[newSprite].extra = 1;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case FLOORFLAME__STATIC:
            pSprite->shade = -127;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case BOUNCEMINE__STATIC:
            pSprite->owner = newSprite;
            pSprite->cstat |= 1+256; //Make it hitable
            pSprite->xrepeat = pSprite->yrepeat = 24;
            pSprite->shade = -127;
            pSprite->extra = g_impactDamage<<2;
            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case STEAM__STATIC:
            if (spriteNum >= 0)
            {
                pSprite->ang = sprite[spriteNum].ang;
                pSprite->cstat = 16+128+2;
                pSprite->xrepeat=pSprite->yrepeat=1;
                pSprite->xvel = -8;
                A_SetSprite(newSprite, CLIPMASK0);
            }
            fallthrough__;
        case CEILINGSTEAM__STATIC:
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case TOILET__STATIC:
        case STALL__STATIC:
            pSprite->lotag = 1;
            pSprite->cstat |= 257;
            pSprite->clipdist = 8;
            pSprite->owner = newSprite;
            goto SPAWN_END;

        case CANWITHSOMETHING__STATIC:
        case CANWITHSOMETHING2__STATIC:
        case CANWITHSOMETHING3__STATIC:
        case CANWITHSOMETHING4__STATIC:
        case RUBBERCAN__STATIC:
            pSprite->extra = 0;
            fallthrough__;
        case EXPLODINGBARREL__STATIC:
        case HORSEONSIDE__STATIC:
        case FIREBARREL__STATIC:
        case NUKEBARREL__STATIC:
        case FIREVASE__STATIC:
        case NUKEBARRELDENTED__STATIC:
        case NUKEBARRELLEAKED__STATIC:
        case WOODENHORSE__STATIC:
            if (spriteNum >= 0)
                pSprite->xrepeat = pSprite->yrepeat = 32;
            pSprite->clipdist = 72;
            A_Fall(newSprite);
            if (spriteNum >= 0)
                pSprite->owner = spriteNum;
            else pSprite->owner = newSprite;
            fallthrough__;
        case EGG__STATIC:
            if (ud.monsters_off == 1 && pSprite->picnum == EGG)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
            }
            else
            {
                if (pSprite->picnum == EGG)
                    pSprite->clipdist = 24;
                pSprite->cstat = 257|(krand()&4);
                changespritestat(newSprite, STAT_ZOMBIEACTOR);
            }
            goto SPAWN_END;

        case TOILETWATER__STATIC:
            pSprite->shade = -16;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case LASERLINE__STATIC:
            pSprite->yrepeat = 6;
            pSprite->xrepeat = 32;

            if (g_tripbombLaserMode == 1)
                pSprite->cstat = 16 + 2;
            else if (g_tripbombLaserMode == 0 || g_tripbombLaserMode == 2)
                pSprite->cstat = 16;
            else
            {
                pSprite->xrepeat = 0;
                pSprite->yrepeat = 0;
            }

            if (spriteNum >= 0) pSprite->ang = actor[spriteNum].t_data[5]+512;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case FORCESPHERE__STATIC:
            if (spriteNum == -1)
            {
                pSprite->cstat = 32768;
                changespritestat(newSprite, STAT_ZOMBIEACTOR);
            }
            else
            {
                pSprite->xrepeat = pSprite->yrepeat = 1;
                changespritestat(newSprite, STAT_MISC);
            }
            goto SPAWN_END;

        case BLOOD__STATIC:
            pSprite->xrepeat = pSprite->yrepeat = 16;
            pSprite->z -= (26<<8);
            if (spriteNum >= 0 && sprite[spriteNum].pal == 6)
                pSprite->pal = 6;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case LAVAPOOL__STATIC:
            if (!WORLDTOUR)
                break;
            fallthrough__;
        case BLOODPOOL__STATIC:
        case PUKE__STATIC:
        {
            int16_t pukeSect = pSprite->sectnum;

            updatesector(pSprite->x + 108, pSprite->y + 108, &pukeSect);
            if (pukeSect >= 0 && sector[pukeSect].floorz == sector[pSprite->sectnum].floorz)
            {
                updatesector(pSprite->x - 108, pSprite->y - 108, &pukeSect);
                if (pukeSect >= 0 && sector[pukeSect].floorz == sector[pSprite->sectnum].floorz)
                {
                    updatesector(pSprite->x + 108, pSprite->y - 108, &pukeSect);
                    if (pukeSect >= 0 && sector[pukeSect].floorz == sector[pSprite->sectnum].floorz)
                    {
                        updatesector(pSprite->x - 108, pSprite->y + 108, &pukeSect);
                        if (pukeSect >= 0 && sector[pukeSect].floorz != sector[pSprite->sectnum].floorz)
                            goto zero_puke;
                    }
                    else goto zero_puke;
                }
                else goto zero_puke;
            }
            else
            {
            zero_puke:
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            if (sector[sectNum].lotag == ST_1_ABOVE_WATER)
            {
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            if (spriteNum >= 0 && pSprite->picnum != PUKE)
            {
                if (sprite[spriteNum].pal == 1)
                    pSprite->pal = 1;
                else if (sprite[spriteNum].pal != 6 && sprite[spriteNum].picnum != NUKEBARREL && sprite[spriteNum].picnum != TIRE)
                    pSprite->pal = (sprite[spriteNum].picnum == FECES) ? 7 : 2;  // Brown or red
                else
                    pSprite->pal = 0;  // green

                if (sprite[spriteNum].picnum == TIRE)
                    pSprite->shade = 127;
            }
            pSprite->cstat |= 32;
            if (pSprite->picnum == LAVAPOOL)
                pSprite->z = getflorzofslope(pSprite->sectnum, pSprite->x, pSprite->y) - 200;
            fallthrough__;
        }
        case FECES__STATIC:
            if (spriteNum >= 0)
                pSprite->xrepeat = pSprite->yrepeat = 1;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case BLOODSPLAT1__STATIC:
        case BLOODSPLAT2__STATIC:
        case BLOODSPLAT3__STATIC:
        case BLOODSPLAT4__STATIC:
            pSprite->cstat |= 16;
            pSprite->xrepeat = 7 + (krand() & 7);
            pSprite->yrepeat = 7 + (krand() & 7);
            pSprite->z += (tilesiz[pSprite->picnum].y * pSprite->yrepeat) >> 2;

            if (spriteNum >= 0 && sprite[spriteNum].pal == 6)
                pSprite->pal = 6;

            A_AddToDeleteQueue(newSprite);
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case TRIPBOMB__STATIC:
            if (pSprite->lotag > ud.player_skill)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            pSprite->xrepeat = 4;
            pSprite->yrepeat = 5;
            pSprite->hitag   = newSprite;
            pSprite->owner   = pSprite->hitag;
            pSprite->xvel    = 16;

            A_SetSprite(newSprite, CLIPMASK0);

            pActor->t_data[0] = 17;
            pActor->t_data[2] = 0;
            pActor->t_data[5] = pSprite->ang;

            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        case SPACEMARINE__STATIC:
            pSprite->extra = 20;
            pSprite->cstat |= 257;
            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;
        case DOORSHOCK__STATIC:
            pSprite->cstat |= 1+256;
            pSprite->shade = -12;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;
        case HYDRENT__STATIC:
        case PANNEL1__STATIC:
        case PANNEL2__STATIC:
        case SATELITE__STATIC:
        case FUELPOD__STATIC:
        case SOLARPANNEL__STATIC:
        case ANTENNA__STATIC:
        case CHAIR1__STATIC:
        case CHAIR2__STATIC:
        case CHAIR3__STATIC:
        case BOTTLE1__STATIC:
        case BOTTLE2__STATIC:
        case BOTTLE3__STATIC:
        case BOTTLE4__STATIC:
        case BOTTLE5__STATIC:
        case BOTTLE6__STATIC:
        case BOTTLE7__STATIC:
        case BOTTLE8__STATIC:
        case BOTTLE10__STATIC:
        case BOTTLE11__STATIC:
        case BOTTLE12__STATIC:
        case BOTTLE13__STATIC:
        case BOTTLE14__STATIC:
        case BOTTLE15__STATIC:
        case BOTTLE16__STATIC:
        case BOTTLE17__STATIC:
        case BOTTLE18__STATIC:
        case BOTTLE19__STATIC:
        case OCEANSPRITE1__STATIC:
        case OCEANSPRITE2__STATIC:
        case OCEANSPRITE3__STATIC:
        case OCEANSPRITE5__STATIC:
        case MONK__STATIC:
        case INDY__STATIC:
        case LUKE__STATIC:
        case JURYGUY__STATIC:
        case SCALE__STATIC:
        case VACUUM__STATIC:
        case CACTUS__STATIC:
        case CACTUSBROKE__STATIC:
        case HANGLIGHT__STATIC:
        case FETUS__STATIC:
        case FETUSBROKE__STATIC:
        case CAMERALIGHT__STATIC:
        case MOVIECAMERA__STATIC:
        case IVUNIT__STATIC:
        case POT1__STATIC:
        case POT2__STATIC:
        case POT3__STATIC:
        case TRIPODCAMERA__STATIC:
        case SUSHIPLATE1__STATIC:
        case SUSHIPLATE2__STATIC:
        case SUSHIPLATE3__STATIC:
        case SUSHIPLATE4__STATIC:
        case SUSHIPLATE5__STATIC:
        case WAITTOBESEATED__STATIC:
        case VASE__STATIC:
        case PIPE1__STATIC:
        case PIPE2__STATIC:
        case PIPE3__STATIC:
        case PIPE4__STATIC:
        case PIPE5__STATIC:
        case PIPE6__STATIC:
#endif
        case GRATE1__STATIC:
        case FANSPRITE__STATIC:
            pSprite->clipdist = 32;
            pSprite->cstat |= 257;
            fallthrough__;
        case OCEANSPRITE4__STATIC:
            changespritestat(newSprite, STAT_DEFAULT);
            goto SPAWN_END;

        case FRAMEEFFECT1_13__STATIC:
            if (PLUTOPAK)
                break;
            fallthrough__;
        case FRAMEEFFECT1__STATIC:
            if (spriteNum >= 0)
            {
                pSprite->xrepeat = sprite[spriteNum].xrepeat;
                pSprite->yrepeat = sprite[spriteNum].yrepeat;
                T2(newSprite) = sprite[spriteNum].picnum;
            }
            else pSprite->xrepeat = pSprite->yrepeat = 0;

            changespritestat(newSprite, STAT_MISC);

            goto SPAWN_END;
        case FOOTPRINTS__STATIC:
        case FOOTPRINTS2__STATIC:
        case FOOTPRINTS3__STATIC:
        case FOOTPRINTS4__STATIC:
            if (spriteNum >= 0)
            {
                int16_t footSect = pSprite->sectnum;

                updatesector(pSprite->x + 84, pSprite->y + 84, &footSect);
                if (footSect >= 0 && sector[footSect].floorz == sector[pSprite->sectnum].floorz)
                {
                    updatesector(pSprite->x - 84, pSprite->y - 84, &footSect);
                    if (footSect >= 0 && sector[footSect].floorz == sector[pSprite->sectnum].floorz)
                    {
                        updatesector(pSprite->x + 84, pSprite->y - 84, &footSect);
                        if (footSect >= 0 && sector[footSect].floorz == sector[pSprite->sectnum].floorz)
                        {
                            updatesector(pSprite->x - 84, pSprite->y + 84, &footSect);
                            if (footSect >= 0 && sector[footSect].floorz != sector[pSprite->sectnum].floorz)
                            {
                                pSprite->xrepeat = pSprite->yrepeat = 0;
                                changespritestat(newSprite, STAT_MISC);
                                goto SPAWN_END;
                            }
                        }
                        else goto zero_footprint;
                    }
                    else goto zero_footprint;
                }
                else
                {
                zero_footprint:
                    pSprite->xrepeat = pSprite->yrepeat = 0;
                    goto SPAWN_END;
                }

                pSprite->cstat = 32 + ((g_player[P_Get(spriteNum)].ps->footprintcount & 1) << 2);
                pSprite->ang   = sprite[spriteNum].ang;
            }

            pSprite->z = sector[sectNum].floorz;

            if (sector[sectNum].lotag != ST_1_ABOVE_WATER && sector[sectNum].lotag != ST_2_UNDERWATER)
                pSprite->xrepeat = pSprite->yrepeat = 32;

            A_AddToDeleteQueue(newSprite);
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;

        case VIEWSCREEN__STATIC:
        case VIEWSCREEN2__STATIC:
            pSprite->owner = newSprite;
            pSprite->lotag = pSprite->extra = 1;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;
        case RESPAWN__STATIC:
            pSprite->extra = 66-13;
            fallthrough__;
        case MUSICANDSFX__STATIC:
            if ((!g_netServer && ud.multimode < 2) && pSprite->pal == 1)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
            pSprite->cstat = 32768;
            changespritestat(newSprite, STAT_FX);
            goto SPAWN_END;

        case EXPLOSION2__STATIC:
#ifdef POLYMER
            if (pSprite->yrepeat > 32)
            {
                G_AddGameLight(0, newSprite, ((pSprite->yrepeat*tilesiz[pSprite->picnum].y)<<1), 32768, 255+(95<<8),PR_LIGHT_PRIO_MAX_GAME);
                pActor->lightcount = 2;
            }
            fallthrough__;
#endif
#ifndef EDUKE32_STANDALONE
        case ONFIRE__STATIC:
            if (!WORLDTOUR && pSprite->picnum == ONFIRE)
                break;
            fallthrough__;
        case EXPLOSION2BOT__STATIC:
        case BURNING__STATIC:
        case BURNING2__STATIC:
        case SMALLSMOKE__STATIC:
        case SHRINKEREXPLOSION__STATIC:
        case COOLEXPLOSION1__STATIC:
#endif
            if (spriteNum >= 0)
            {
                pSprite->ang = sprite[spriteNum].ang;
                pSprite->shade = -64;
                pSprite->cstat = 128|(krand()&4);
            }

            if (pSprite->picnum == EXPLOSION2 || pSprite->picnum == EXPLOSION2BOT)
            {
                pSprite->xrepeat = pSprite->yrepeat = 48;
                pSprite->shade = -127;
                pSprite->cstat |= 128;
            }
            else if (pSprite->picnum == SHRINKEREXPLOSION)
                pSprite->xrepeat = pSprite->yrepeat = 32;
            else if (pSprite->picnum == SMALLSMOKE || pSprite->picnum == ONFIRE)
            {
                // 64 "money"
                pSprite->xrepeat = pSprite->yrepeat = 24;
            }
            else if (pSprite->picnum == BURNING || pSprite->picnum == BURNING2)
                pSprite->xrepeat = pSprite->yrepeat = 4;

            pSprite->cstat |= 8192;

            if (spriteNum >= 0)
            {
                int const floorZ = getflorzofslope(pSprite->sectnum, pSprite->x, pSprite->y);

                if (pSprite->z > floorZ-ZOFFSET4)
                    pSprite->z = floorZ-ZOFFSET4;
            }

            if (pSprite->picnum == ONFIRE)
            {
                pActor->bpos.x = pSprite->x += (krand()%256)-128;
                pActor->bpos.y = pSprite->y += (krand()%256)-128;
                pActor->bpos.z = pSprite->z -= krand()%10240;
                pSprite->cstat |= 128;
            }

            changespritestat(newSprite, STAT_MISC);

            goto SPAWN_END;

        case PLAYERONWATER__STATIC:
            if (spriteNum >= 0)
            {
                pSprite->xrepeat = sprite[spriteNum].xrepeat;
                pSprite->yrepeat = sprite[spriteNum].yrepeat;
                pSprite->zvel = 128;
                if (sector[pSprite->sectnum].lotag != ST_2_UNDERWATER)
                    pSprite->cstat |= 32768;
            }
            changespritestat(newSprite, STAT_DUMMYPLAYER);
            goto SPAWN_END;

        case APLAYER__STATIC:
            pSprite->xrepeat = 0;
            pSprite->yrepeat = 0;
            pSprite->cstat   = 32768;

            changespritestat(newSprite, ((!g_netServer && ud.multimode < 2)
                                         || ((g_gametypeFlags[ud.coop] & GAMETYPE_COOPSPAWN) / GAMETYPE_COOPSPAWN) != pSprite->lotag)
                                        ? STAT_MISC
                                        : STAT_PLAYER);
            goto SPAWN_END;
        case TOUCHPLATE__STATIC:
            T3(newSprite) = sector[sectNum].floorz;

            if (sector[sectNum].lotag != ST_1_ABOVE_WATER && sector[sectNum].lotag != ST_2_UNDERWATER)
                sector[sectNum].floorz = pSprite->z;

            if (pSprite->pal && (g_netServer || ud.multimode > 1))
            {
                pSprite->xrepeat=pSprite->yrepeat=0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }
#ifndef EDUKE32_STANDALONE
            fallthrough__;
        case WATERBUBBLEMAKER__STATIC:
            if (EDUKE32_PREDICT_FALSE(pSprite->hitag && pSprite->picnum == WATERBUBBLEMAKER))
            {
                // JBF 20030913: Pisses off X_Move(), eg. in bobsp2
                Printf(TEXTCOLOR_RED "WARNING: WATERBUBBLEMAKER %d @ %d,%d with hitag!=0. Applying fixup.\n",
                           newSprite,TrackerCast(pSprite->x),TrackerCast(pSprite->y));
                pSprite->hitag = 0;
            }
#endif
            pSprite->cstat |= 32768;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case MASTERSWITCH__STATIC:
            if (pSprite->picnum == MASTERSWITCH)
                pSprite->cstat |= 32768;
            pSprite->yvel = 0;
            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;
        case LOCATORS__STATIC:
            pSprite->cstat |= 32768;
            changespritestat(newSprite, STAT_LOCATOR);
            goto SPAWN_END;

        case ACTIVATORLOCKED__STATIC:
        case ACTIVATOR__STATIC:
            pSprite->cstat = 32768;
            if (pSprite->picnum == ACTIVATORLOCKED)
                sector[pSprite->sectnum].lotag |= 16384;
            changespritestat(newSprite, STAT_ACTIVATOR);
            goto SPAWN_END;

        case OOZ__STATIC:
        case OOZ2__STATIC:
        {
            pSprite->shade = -12;

            if (spriteNum >= 0)
            {
                if (sprite[spriteNum].picnum == NUKEBARREL)
                    pSprite->pal = 8;
                A_AddToDeleteQueue(newSprite);
            }

            changespritestat(newSprite, STAT_ACTOR);

            A_GetZLimits(newSprite);

            int const oozSize = (pActor->floorz-pActor->ceilingz)>>9;

            pSprite->yrepeat = oozSize;
            pSprite->xrepeat = 25 - (oozSize >> 1);
            pSprite->cstat |= (krand() & 4);

            goto SPAWN_END;
        }

        case SECTOREFFECTOR__STATIC:
            pSprite->cstat |= 32768;
            pSprite->xrepeat = pSprite->yrepeat = 0;

            switch (pSprite->lotag)
            {
#ifdef LEGACY_ROR
            case 40:
            case 41:
                pSprite->cstat = 32;
                pSprite->xrepeat = pSprite->yrepeat = 64;
                changespritestat(newSprite, STAT_EFFECTOR);
                for (spriteNum=0; spriteNum < MAXSPRITES; spriteNum++)
                    if (sprite[spriteNum].picnum == SECTOREFFECTOR && (sprite[spriteNum].lotag == 40 || sprite[spriteNum].lotag == 41) &&
                            sprite[spriteNum].hitag == pSprite->hitag && newSprite != spriteNum)
                    {
//                        Printf("found ror match\n");
                        pSprite->yvel = spriteNum;
                        break;
                    }
                goto SPAWN_END;
                break;
            case 46:
                ror_protectedsectors[pSprite->sectnum] = 1;
                /* XXX: fall-through intended? */
                fallthrough__;
#endif
            case SE_49_POINT_LIGHT:
            case SE_50_SPOT_LIGHT:
            {
                int32_t j, nextj;

                for (TRAVERSE_SPRITE_SECT(headspritesect[pSprite->sectnum], j, nextj))
                    if (sprite[j].picnum == ACTIVATOR || sprite[j].picnum == ACTIVATORLOCKED)
                        pActor->flags |= SFLAG_USEACTIVATOR;
            }
            changespritestat(newSprite, pSprite->lotag==46 ? STAT_EFFECTOR : STAT_LIGHT);
            goto SPAWN_END;
            break;
            }

            pSprite->yvel = sector[sectNum].extra;

            switch (pSprite->lotag)
            {
            case SE_28_LIGHTNING:
                T6(newSprite) = 65;// Delay for lightning
                break;
            case SE_7_TELEPORT: // Transporters!!!!
            case SE_23_ONE_WAY_TELEPORT:// XPTR END
                if (pSprite->lotag != SE_23_ONE_WAY_TELEPORT)
                {
                    for (spriteNum=0; spriteNum<MAXSPRITES; spriteNum++)
                        if (sprite[spriteNum].statnum < MAXSTATUS && sprite[spriteNum].picnum == SECTOREFFECTOR &&
                                (sprite[spriteNum].lotag == SE_7_TELEPORT || sprite[spriteNum].lotag == SE_23_ONE_WAY_TELEPORT) && newSprite != spriteNum && sprite[spriteNum].hitag == SHT(newSprite))
                        {
                            OW(newSprite) = spriteNum;
                            break;
                        }
                }
                else OW(newSprite) = newSprite;

                T5(newSprite) = (sector[sectNum].floorz == SZ(newSprite));  // ONFLOORZ
                pSprite->cstat = 0;
                changespritestat(newSprite, STAT_TRANSPORT);
                goto SPAWN_END;
            case SE_1_PIVOT:
                pSprite->owner = -1;
                T1(newSprite) = 1;
                break;
            case SE_18_INCREMENTAL_SECTOR_RISE_FALL:

                if (pSprite->ang == 512)
                {
                    T2(newSprite) = sector[sectNum].ceilingz;
                    if (pSprite->pal)
                        sector[sectNum].ceilingz = pSprite->z;
                }
                else
                {
                    T2(newSprite) = sector[sectNum].floorz;
                    if (pSprite->pal)
                        sector[sectNum].floorz = pSprite->z;
                }

                pSprite->hitag <<= 2;
                break;

            case SE_19_EXPLOSION_LOWERS_CEILING:
                pSprite->owner = -1;
                break;
            case SE_25_PISTON: // Pistons
                T4(newSprite) = sector[sectNum].ceilingz;
                T5(newSprite) = 1;
                sector[sectNum].ceilingz = pSprite->z;
                G_SetInterpolation(&sector[sectNum].ceilingz);
                break;
            case SE_35:
                sector[sectNum].ceilingz = pSprite->z;
                break;
            case SE_27_DEMO_CAM:
                T1(newSprite) = 0;
                if (ud.recstat == 1)
                {
                    pSprite->xrepeat=pSprite->yrepeat=64;
                    pSprite->cstat &= 32768;
                }
                break;
            case SE_12_LIGHT_SWITCH:

                T2(newSprite) = sector[sectNum].floorshade;
                T3(newSprite) = sector[sectNum].ceilingshade;
                break;

            case SE_13_EXPLOSIVE:

                T1(newSprite) = sector[sectNum].ceilingz;
                T2(newSprite) = sector[sectNum].floorz;

                if (klabs(T1(newSprite)-pSprite->z) < klabs(T2(newSprite)-pSprite->z))
                    pSprite->owner = 1;
                else pSprite->owner = 0;

                if (pSprite->ang == 512)
                {
                    if (pSprite->owner)
                        sector[sectNum].ceilingz = pSprite->z;
                    else
                        sector[sectNum].floorz = pSprite->z;
#ifdef YAX_ENABLE
                    {
                        int16_t cf=!pSprite->owner, bn=yax_getbunch(sectNum, cf);
                        int32_t jj, daz=SECTORFLD(sectNum,z, cf);

                        if (bn >= 0)
                        {
                            for (SECTORS_OF_BUNCH(bn, cf, jj))
                            {
                                SECTORFLD(jj,z, cf) = daz;
                                SECTORFLD(jj,stat, cf) &= ~256;
                                SECTORFLD(jj,stat, cf) |= 128 + 512+2048;
                            }
                            for (SECTORS_OF_BUNCH(bn, !cf, jj))
                            {
                                SECTORFLD(jj,z, !cf) = daz;
                                SECTORFLD(jj,stat, !cf) &= ~256;
                                SECTORFLD(jj,stat, !cf) |= 128 + 512+2048;
                            }
                        }
                    }
#endif
                }
                else
                    sector[sectNum].ceilingz = sector[sectNum].floorz = pSprite->z;

                if (sector[sectNum].ceilingstat&1)
                {
                    sector[sectNum].ceilingstat ^= 1;
                    T4(newSprite) = 1;

                    if (!pSprite->owner && pSprite->ang==512)
                    {
                        sector[sectNum].ceilingstat ^= 1;
                        T4(newSprite) = 0;
                    }

                    sector[sectNum].ceilingshade =
                        sector[sectNum].floorshade;

                    if (pSprite->ang==512)
                    {
                        int const startwall = sector[sectNum].wallptr;
                        int const endwall   = startwall + sector[sectNum].wallnum;
                        for (bssize_t j = startwall; j < endwall; j++)
                        {
                            int const nextSect = wall[j].nextsector;

                            if (nextSect >= 0)
                            {
                                if (!(sector[nextSect].ceilingstat & 1))
                                {
                                    sector[sectNum].ceilingpicnum = sector[nextSect].ceilingpicnum;
                                    sector[sectNum].ceilingshade  = sector[nextSect].ceilingshade;
                                    break;  // Leave earily
                                }
                            }
                        }
                    }
                }

                break;

            case SE_17_WARP_ELEVATOR:
            {
                T3(newSprite) = sector[sectNum].floorz;  // Stopping loc

                int nextSectNum = nextsectorneighborz(sectNum, sector[sectNum].floorz, -1, -1);

                if (EDUKE32_PREDICT_TRUE(nextSectNum >= 0))
                    T4(newSprite) = sector[nextSectNum].ceilingz;
                else
                {
                    // use elevator sector's ceiling as heuristic
                    T4(newSprite) = sector[sectNum].ceilingz;

                    Printf(TEXTCOLOR_RED "WARNING: SE17 sprite %d using own sector's ceilingz to "
                                         "determine when to warp. Sector %d adjacent to a door?\n",
                               newSprite, sectNum);
                }

                nextSectNum = nextsectorneighborz(sectNum, sector[sectNum].ceilingz, 1, 1);

                if (EDUKE32_PREDICT_TRUE(nextSectNum >= 0))
                    T5(newSprite) = sector[nextSectNum].floorz;
                else
                {
                    // heuristic
                    T5(newSprite) = sector[sectNum].floorz;

                    Printf(TEXTCOLOR_RED "WARNING: SE17 sprite %d using own sector %d's floorz.\n",
                               newSprite, sectNum);
                }

                if (numplayers < 2 && !g_netServer)
                {
                    G_SetInterpolation(&sector[sectNum].floorz);
                    G_SetInterpolation(&sector[sectNum].ceilingz);
                }
            }
            break;

            case SE_24_CONVEYOR:
                pSprite->yvel <<= 1;
            case SE_36_PROJ_SHOOTER:
                break;

            case SE_20_STRETCH_BRIDGE:
            {
                int       closestDist = INT32_MAX;
                int       closestWall = 0;
                int const startWall   = sector[sectNum].wallptr;
                int const endWall     = startWall + sector[sectNum].wallnum;

                for (bssize_t findWall=startWall; findWall<endWall; findWall++)
                {
                    int const x = wall[findWall].x;
                    int const y = wall[findWall].y;
                    int const d = FindDistance2D(pSprite->x - x, pSprite->y - y);

                    if (d < closestDist)
                    {
                        closestDist = d;
                        closestWall = findWall;
                    }
                }

                T2(newSprite) = closestWall;

                closestDist = INT32_MAX;

                for (bssize_t findWall=startWall; findWall<endWall; findWall++)
                {
                    int const x = wall[findWall].x;
                    int const y = wall[findWall].y;
                    int const d = FindDistance2D(pSprite->x - x, pSprite->y - y);

                    if (d < closestDist && findWall != T2(newSprite))
                    {
                        closestDist = d;
                        closestWall = findWall;
                    }
                }

                T3(newSprite) = closestWall;
            }

            break;

            case SE_3_RANDOM_LIGHTS_AFTER_SHOT_OUT:
            {

                T4(newSprite)=sector[sectNum].floorshade;

                sector[sectNum].floorshade   = pSprite->shade;
                sector[sectNum].ceilingshade = pSprite->shade;

                pSprite->owner = sector[sectNum].ceilingpal << 8;
                pSprite->owner |= sector[sectNum].floorpal;

                //fix all the walls;

                int const startWall = sector[sectNum].wallptr;
                int const endWall = startWall+sector[sectNum].wallnum;

                for (bssize_t w=startWall; w<endWall; ++w)
                {
                    if (!(wall[w].hitag & 1))
                        wall[w].shade = pSprite->shade;

                    if ((wall[w].cstat & 2) && wall[w].nextwall >= 0)
                        wall[wall[w].nextwall].shade = pSprite->shade;
                }
                break;
            }

            case SE_31_FLOOR_RISE_FALL:
            {
                T2(newSprite) = sector[sectNum].floorz;

                if (pSprite->ang != 1536)
                {
                    sector[sectNum].floorz = pSprite->z;
                    Yax_SetBunchZs(sectNum, YAX_FLOOR, pSprite->z);
                }

                int const startWall = sector[sectNum].wallptr;
                int const endWall   = startWall + sector[sectNum].wallnum;

                for (bssize_t w = startWall; w < endWall; ++w)
                    if (wall[w].hitag == 0)
                        wall[w].hitag = 9999;

                G_SetInterpolation(&sector[sectNum].floorz);
                Yax_SetBunchInterpolation(sectNum, YAX_FLOOR);
            }
            break;

            case SE_32_CEILING_RISE_FALL:
            {
                T2(newSprite) = sector[sectNum].ceilingz;
                T3(newSprite) = pSprite->hitag;

                if (pSprite->ang != 1536)
                {
                    sector[sectNum].ceilingz = pSprite->z;
                    Yax_SetBunchZs(sectNum, YAX_CEILING, pSprite->z);
                }

                int const startWall = sector[sectNum].wallptr;
                int const endWall   = startWall + sector[sectNum].wallnum;

                for (bssize_t w = startWall; w < endWall; ++w)
                    if (wall[w].hitag == 0)
                        wall[w].hitag = 9999;

                G_SetInterpolation(&sector[sectNum].ceilingz);
                Yax_SetBunchInterpolation(sectNum, YAX_CEILING);
            }
            break;

            case SE_4_RANDOM_LIGHTS: //Flashing lights
            {
                T3(newSprite) = sector[sectNum].floorshade;

                int const startWall = sector[sectNum].wallptr;
                int const endWall   = startWall + sector[sectNum].wallnum;

                pSprite->owner = sector[sectNum].ceilingpal << 8;
                pSprite->owner |= sector[sectNum].floorpal;

                for (bssize_t w = startWall; w < endWall; ++w)
                    if (wall[w].shade > T4(newSprite))
                        T4(newSprite) = wall[w].shade;
            }
            break;

            case SE_9_DOWN_OPEN_DOOR_LIGHTS:
                if (sector[sectNum].lotag &&
                        labs(sector[sectNum].ceilingz-pSprite->z) > 1024)
                    sector[sectNum].lotag |= 32768u; //If its open
                fallthrough__;
            case SE_8_UP_OPEN_DOOR_LIGHTS:
                //First, get the ceiling-floor shade
                {
                    T1(newSprite) = sector[sectNum].floorshade;
                    T2(newSprite) = sector[sectNum].ceilingshade;

                    int const startWall = sector[sectNum].wallptr;
                    int const endWall   = startWall + sector[sectNum].wallnum;

                    for (bssize_t w = startWall; w < endWall; ++w)
                        if (wall[w].shade > T3(newSprite))
                            T3(newSprite) = wall[w].shade;

                    T4(newSprite) = 1;  // Take Out;
                }
                break;

            case SE_11_SWINGING_DOOR:  // Pivitor rotater
                T4(newSprite) = (pSprite->ang > 1024) ? 2 : -2;
                fallthrough__;
            case SE_0_ROTATING_SECTOR:
            case SE_2_EARTHQUAKE:      // Earthquakemakers
            case SE_5:                 // Boss Creature
            case SE_6_SUBWAY:          // Subway
            case SE_14_SUBWAY_CAR:     // Caboos
            case SE_15_SLIDING_DOOR:   // Subwaytype sliding door
            case SE_16_REACTOR:        // That rotating blocker reactor thing
            case SE_26:                // ESCELATOR
            case SE_30_TWO_WAY_TRAIN:  // No rotational subways
                if (pSprite->lotag == SE_0_ROTATING_SECTOR)
                {
                    if (sector[sectNum].lotag == ST_30_ROTATE_RISE_BRIDGE)
                    {
                        sprite[newSprite].clipdist = (pSprite->pal) ? 1 : 0;
                        T4(newSprite) = sector[sectNum].floorz;
                        sector[sectNum].hitag = newSprite;
                    }

                    for (spriteNum = MAXSPRITES-1; spriteNum>=0; spriteNum--)
                    {
                        if (sprite[spriteNum].statnum < MAXSTATUS)
                            if (sprite[spriteNum].picnum == SECTOREFFECTOR &&
                                    sprite[spriteNum].lotag == SE_1_PIVOT &&
                                    sprite[spriteNum].hitag == pSprite->hitag)
                            {
                                if (pSprite->ang == 512)
                                {
                                    pSprite->x = sprite[spriteNum].x;
                                    pSprite->y = sprite[spriteNum].y;
                                }
                                break;
                            }
                    }
                    if (EDUKE32_PREDICT_FALSE(spriteNum == -1))
                    {
                        Printf(TEXTCOLOR_RED "Found lonely Sector Effector (lotag 0) at (%d,%d)\n",
                            TrackerCast(pSprite->x),TrackerCast(pSprite->y));
                        changespritestat(newSprite, STAT_ACTOR);
                        goto SPAWN_END;
                    }
                    pSprite->owner = spriteNum;
                }

                {
                    int const startWall = sector[sectNum].wallptr;
                    int const endWall = startWall+sector[sectNum].wallnum;

                    T2(newSprite) = tempwallptr;
                    for (bssize_t w = startWall; w < endWall; ++w)
                    {
                        g_origins[tempwallptr].x = wall[w].x - pSprite->x;
                        g_origins[tempwallptr].y = wall[w].y - pSprite->y;

                        tempwallptr++;
                        if (EDUKE32_PREDICT_FALSE(tempwallptr >= MAXANIMPOINTS))
                        {
                            Bsprintf(tempbuf, "Too many moving sectors at (%d,%d).\n",
                                TrackerCast(wall[w].x), TrackerCast(wall[w].y));
                            G_GameExit(tempbuf);
                        }
                    }
                }

                if (pSprite->lotag == SE_5 || pSprite->lotag == SE_30_TWO_WAY_TRAIN ||
                        pSprite->lotag == SE_6_SUBWAY || pSprite->lotag == SE_14_SUBWAY_CAR)
                {
#ifdef YAX_ENABLE
                    int outerWall = -1;
#endif
                    int const startWall = sector[sectNum].wallptr;
                    int const endWall   = startWall + sector[sectNum].wallnum;

                    pSprite->extra = (uint16_t)((uint16_t)sector[sectNum].hitag != UINT16_MAX);

                    // TRAIN_SECTOR_TO_SE_INDEX
                    sector[sectNum].hitag = newSprite;

                    spriteNum = 0;

                    int foundWall = startWall;

                    for (; foundWall<endWall; foundWall++)
                    {
                        if (wall[ foundWall ].nextsector >= 0 &&
                                sector[ wall[ foundWall ].nextsector].hitag == 0 &&
                                (int16_t)sector[ wall[ foundWall ].nextsector].lotag < 3)
                        {
#ifdef YAX_ENABLE
                            outerWall = wall[foundWall].nextwall;
#endif
                            foundWall = wall[foundWall].nextsector;
                            spriteNum = 1;
                            break;
                        }
                    }

#ifdef YAX_ENABLE
                    pActor->t_data[9] = -1;

                    if (outerWall >= 0)
                    {
                        int upperSect = yax_vnextsec(outerWall, YAX_CEILING);

                        if (upperSect >= 0)
                        {
                            int foundEffector = headspritesect[upperSect];

                            for (; foundEffector >= 0; foundEffector = nextspritesect[foundEffector])
                                if (sprite[foundEffector].picnum == SECTOREFFECTOR && sprite[foundEffector].lotag == pSprite->lotag)
                                    break;

                            if (foundEffector < 0)
                            {
                                Sect_SetInterpolation(upperSect);
                                pActor->t_data[9] = upperSect;
                            }
                        }
                    }
#endif
                    if (spriteNum == 0)
                    {
                        Bsprintf(tempbuf,"Subway found no zero'd sectors with locators\nat (%d,%d).\n",
                            TrackerCast(pSprite->x),TrackerCast(pSprite->y));
                        G_GameExit(tempbuf);
                    }

                    pSprite->owner = -1;
                    T1(newSprite) = foundWall;

                    if (pSprite->lotag != SE_30_TWO_WAY_TRAIN)
                        T4(newSprite) = pSprite->hitag;
                }
                else if (pSprite->lotag == SE_16_REACTOR)
                    T4(newSprite) = sector[sectNum].ceilingz;
                else if (pSprite->lotag == SE_26)
                {
                    T4(newSprite)  = pSprite->x;
                    T5(newSprite)  = pSprite->y;
                    pSprite->zvel  = (pSprite->shade == sector[sectNum].floorshade) ? -256 : 256;  // UP
                    pSprite->shade = 0;
                }
                else if (pSprite->lotag == SE_2_EARTHQUAKE)
                {
                    T6(newSprite) = sector[pSprite->sectnum].floorheinum;
                    sector[pSprite->sectnum].floorheinum = 0;
                }
            }

            switch (pSprite->lotag)
            {
                case SE_6_SUBWAY:
                case SE_14_SUBWAY_CAR:
                    S_FindMusicSFX(sectNum, &spriteNum);
                    // XXX: uh.. what?
                    if (spriteNum == -1)
                        spriteNum = SUBWAY;
                    pActor->lastv.x = spriteNum;
                    fallthrough__;
                case SE_30_TWO_WAY_TRAIN:
                    if (g_netServer || numplayers > 1)
                        break;
                    fallthrough__;
                case SE_0_ROTATING_SECTOR:
                case SE_1_PIVOT:
                case SE_5:
                case SE_11_SWINGING_DOOR:
                case SE_15_SLIDING_DOOR:
                case SE_16_REACTOR:
                case SE_26: Sect_SetInterpolation(sprite[newSprite].sectnum); break;
            }

            changespritestat(newSprite, STAT_EFFECTOR);
            goto SPAWN_END;

        case SEENINE__STATIC:
        case OOZFILTER__STATIC:
            pSprite->shade = -16;
            if (pSprite->xrepeat <= 8)
            {
                pSprite->cstat   = 32768;
                pSprite->xrepeat = 0;
                pSprite->yrepeat = 0;
            }
            else pSprite->cstat = 1+256;

            pSprite->extra = g_impactDamage << 2;
            pSprite->owner = newSprite;

            changespritestat(newSprite, STAT_STANDABLE);
            goto SPAWN_END;

        case CRACK1__STATIC:
        case CRACK2__STATIC:
        case CRACK3__STATIC:
        case CRACK4__STATIC:
        case FIREEXT__STATIC:
            if (pSprite->picnum == FIREEXT)
            {
                pSprite->cstat = 257;
                pSprite->extra = g_impactDamage<<2;
            }
            else
            {
                pSprite->cstat |= (pSprite->cstat & 48) ? 1 : 17;
                pSprite->extra = 1;
            }

            if ((!g_netServer && ud.multimode < 2) && pSprite->pal != 0)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            pSprite->pal   = 0;
            pSprite->owner = newSprite;
            pSprite->xvel  = 8;

            changespritestat(newSprite, STAT_STANDABLE);
            A_SetSprite(newSprite,CLIPMASK0);
            goto SPAWN_END;

        case LAVAPOOLBUBBLE__STATIC:
            if (!WORLDTOUR)
                break;
            if (sprite[spriteNum].xrepeat >= 30)
            {
                pSprite->owner = spriteNum;
                changespritestat(newSprite, STAT_MISC);
                pSprite->xrepeat = pSprite->yrepeat = 1;
                pSprite->x += (krand()%512)-256;
                pSprite->y += (krand()%512)-256;
            }
            goto SPAWN_END;
        case WHISPYSMOKE__STATIC:
            if (!WORLDTOUR)
                break;
            pActor->bpos.x = pSprite->x += (krand()%256)-128;
            pActor->bpos.y = pSprite->y += (krand()%256)-128;
            pSprite->xrepeat = pSprite->yrepeat = 20;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;
        case FIREFLYFLYINGEFFECT__STATIC:
            if (!WORLDTOUR)
                break;
            pSprite->owner = spriteNum;
            changespritestat(newSprite, STAT_MISC);
            pSprite->xrepeat = pSprite->yrepeat = 1;
            goto SPAWN_END;
        case E32_TILE5846__STATIC:
            if (!WORLDTOUR)
                break;
            pSprite->extra = 150;
            pSprite->cstat |= 257;
            changespritestat(newSprite, STAT_ZOMBIEACTOR);
            goto SPAWN_END;

        default:
            break; // NOT goto
        }

    // implementation of the default case
    if (G_TileHasActor(pSprite->picnum))
    {
        if (spriteNum == -1 && pSprite->lotag > ud.player_skill)
        {
            pSprite->xrepeat = pSprite->yrepeat = 0;
            changespritestat(newSprite, STAT_MISC);
            goto SPAWN_END;
        }

        //  Init the size
        if (pSprite->xrepeat == 0 || pSprite->yrepeat == 0)
            pSprite->xrepeat = pSprite->yrepeat = 1;

        if (A_CheckSpriteFlags(newSprite, SFLAG_BADGUY))
        {
            if (ud.monsters_off == 1)
            {
                pSprite->xrepeat = pSprite->yrepeat = 0;
                changespritestat(newSprite, STAT_MISC);
                goto SPAWN_END;
            }

            A_Fall(newSprite);

            if (A_CheckSpriteFlags(newSprite, SFLAG_BADGUYSTAYPUT))
                pActor->stayput = pSprite->sectnum;

            g_player[myconnectindex].ps->max_actors_killed++;
            pSprite->clipdist = 80;

            if (spriteNum >= 0)
            {
                if (sprite[spriteNum].picnum == RESPAWN)
                    pActor->tempang = sprite[newSprite].pal = sprite[spriteNum].pal;

                A_PlayAlertSound(newSprite);
                changespritestat(newSprite, STAT_ACTOR);
            }
            else
                changespritestat(newSprite, STAT_ZOMBIEACTOR);
        }
        else
        {
            pSprite->clipdist = 40;
            pSprite->owner    = newSprite;
            changespritestat(newSprite, STAT_ACTOR);
        }

        pActor->timetosleep = 0;

        if (spriteNum >= 0)
            pSprite->ang = sprite[spriteNum].ang;
    }

SPAWN_END:
    if (VM_HaveEvent(EVENT_SPAWN))
    {
        int32_t p;
        int32_t pl=A_FindPlayer(&sprite[newSprite],&p);
        VM_ExecuteEvent(EVENT_SPAWN,newSprite, pl, p);
    }

    return newSprite;
}

static int G_MaybeTakeOnFloorPal(tspriteptr_t pSprite, int sectNum)
{
    int const floorPal = sector[sectNum].floorpal;

    if (floorPal && !lookups.noFloorPal(floorPal) && !A_CheckSpriteFlags(pSprite->owner, SFLAG_NOPAL))
    {
        pSprite->pal = floorPal;
        return 1;
    }

    return 0;
}

template <int rotations>
static int getofs_viewtype(int angDiff)
{
    return ((((angDiff + 3072) & 2047) * rotations + 1024) >> 11) % rotations;
}

template <int rotations>
static int viewtype_mirror(uint16_t & cstat, int frameOffset)
{
    if (frameOffset > rotations / 2)
    {
        cstat |= 4;
        return rotations - frameOffset;
    }

    cstat &= ~4;
    return frameOffset;
}

template <int mirrored_rotations>
static int getofs_viewtype_mirrored(uint16_t & cstat, int angDiff)
{
    return viewtype_mirror<mirrored_rotations*2-2>(cstat, getofs_viewtype<mirrored_rotations*2-2>(angDiff));
}

// XXX: this fucking sucks and needs to be replaced with a SFLAG
#ifndef EDUKE32_STANDALONE
static int G_CheckAdultTile(int tileNum)
{
    UNREFERENCED_PARAMETER(tileNum);
    switch (tileNum)
    {
        case FEM1__STATIC:
        case FEM2__STATIC:
        case FEM3__STATIC:
        case FEM4__STATIC:
        case FEM5__STATIC:
        case FEM6__STATIC:
        case FEM7__STATIC:
        case FEM8__STATIC:
        case FEM9__STATIC:
        case FEM10__STATIC:
        case MAN__STATIC:
        case MAN2__STATIC:
        case WOMAN__STATIC:
        case NAKED1__STATIC:
        case PODFEM1__STATIC:
        case FEMMAG1__STATIC:
        case FEMMAG2__STATIC:
        case FEMPIC1__STATIC:
        case FEMPIC2__STATIC:
        case FEMPIC3__STATIC:
        case FEMPIC4__STATIC:
        case FEMPIC5__STATIC:
        case FEMPIC6__STATIC:
        case FEMPIC7__STATIC:
        case BLOODYPOLE__STATIC:
        case FEM6PAD__STATIC:
        case STATUE__STATIC:
        case STATUEFLASH__STATIC:
        case OOZ__STATIC:
        case OOZ2__STATIC:
        case WALLBLOOD1__STATIC:
        case WALLBLOOD2__STATIC:
        case WALLBLOOD3__STATIC:
        case WALLBLOOD4__STATIC:
        case WALLBLOOD5__STATIC:
        case WALLBLOOD7__STATIC:
        case WALLBLOOD8__STATIC:
        case SUSHIPLATE1__STATIC:
        case SUSHIPLATE2__STATIC:
        case SUSHIPLATE3__STATIC:
        case SUSHIPLATE4__STATIC:
        case FETUS__STATIC:
        case FETUSJIB__STATIC:
        case FETUSBROKE__STATIC:
        case HOTMEAT__STATIC:
        case FOODOBJECT16__STATIC:
        case DOLPHIN1__STATIC:
        case DOLPHIN2__STATIC:
        case TOUGHGAL__STATIC:
        case TAMPON__STATIC:
        case XXXSTACY__STATIC:
        case 4946:
        case 4947:
        case 693:
        case 2254:
        case 4560:
        case 4561:
        case 4562:
        case 4498:
        case 4957:
            return 1;
    }
    return 0;
}
#endif

static inline void G_DoEventAnimSprites(int tspriteNum)
{
    int const tsprOwner = tsprite[tspriteNum].owner;

    if ((((unsigned)tsprOwner >= MAXSPRITES || (spriteext[tsprOwner].flags & SPREXT_TSPRACCESS) != SPREXT_TSPRACCESS))
        && tsprite[tspriteNum].statnum != TSPR_TEMP)
        return;

    spriteext[tsprOwner].tspr = &tsprite[tspriteNum];
    VM_ExecuteEvent(EVENT_ANIMATESPRITES, tsprOwner, screenpeek);
    spriteext[tsprOwner].tspr = NULL;
}

void G_DoSpriteAnimations(int32_t ourx, int32_t oury, int32_t ourz, int32_t oura, int32_t smoothratio)
{
    UNREFERENCED_PARAMETER(ourz);
    int32_t j, frameOffset, playerNum;
    intptr_t l;

    if (spritesortcnt == 0)
    {
#ifdef DEBUGGINGAIDS
        g_spriteStat.numonscreen = 0;
#endif
        return;
    }
#ifdef LEGACY_ROR
    ror_sprite = -1;
#endif
    for (j=spritesortcnt-1; j>=0; j--)
    {
        auto const t = &tsprite[j];
        const int32_t i = t->owner;
        auto const s = &sprite[i];

        Duke_ApplySpritePropertiesToTSprite(t, (uspriteptr_t)s);

        switch (DYNAMICTILEMAP(s->picnum))
        {
        case SECTOREFFECTOR__STATIC:
            if (s->lotag == 40 || s->lotag == 41)
            {
                t->cstat = 32768;
#ifdef LEGACY_ROR
                if (ror_sprite == -1)
                    ror_sprite = i;
#endif
            }

            if (t->lotag == SE_27_DEMO_CAM && ud.recstat == 1)
            {
                t->picnum = 11+(((int) totalclock>>3)&1);
                t->cstat |= 128;
            }
            else
                t->xrepeat = t->yrepeat = 0;
            break;
        }
    }

    for (j=spritesortcnt-1; j>=0; j--)
    {
        auto const t = &tsprite[j];
        const int32_t i = t->owner;
        auto const s = (uspriteptr_t)&sprite[i];

        if (t->picnum < GREENSLIME || t->picnum > GREENSLIME+7)
            switch (DYNAMICTILEMAP(t->picnum))
            {
            case BLOODPOOL__STATIC:
            case PUKE__STATIC:
            case FOOTPRINTS__STATIC:
            case FOOTPRINTS2__STATIC:
            case FOOTPRINTS3__STATIC:
            case FOOTPRINTS4__STATIC:
                if (t->shade == 127) continue;
                break;
            case RESPAWNMARKERRED__STATIC:
            case RESPAWNMARKERYELLOW__STATIC:
            case RESPAWNMARKERGREEN__STATIC:
                if (ud.marker == 0)
                    t->xrepeat = t->yrepeat = 0;
                continue;
            case CHAIR3__STATIC:
                if (tilehasmodelorvoxel(t->picnum,t->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
                {
                    t->cstat &= ~4;
                    break;
                }
                frameOffset = getofs_viewtype_mirrored<5>(t->cstat, t->ang - oura);
                t->picnum = s->picnum+frameOffset;
                break;
            case BLOODSPLAT1__STATIC:
            case BLOODSPLAT2__STATIC:
            case BLOODSPLAT3__STATIC:
            case BLOODSPLAT4__STATIC:
                if (adult_lockout) t->xrepeat = t->yrepeat = 0;
                else if (t->pal == 6)
                {
                    t->shade = -127;
                    continue;
                }
                fallthrough__;
            case BULLETHOLE__STATIC:
            case CRACK1__STATIC:
            case CRACK2__STATIC:
            case CRACK3__STATIC:
            case CRACK4__STATIC:
                t->shade = 16;
                continue;
            case NEON1__STATIC:
            case NEON2__STATIC:
            case NEON3__STATIC:
            case NEON4__STATIC:
            case NEON5__STATIC:
            case NEON6__STATIC:
                continue;
            default:
                // NOTE: wall-aligned sprites will never take on ceiling/floor shade...
                if ((t->cstat&16) || (A_CheckEnemySprite(t) &&
                    (unsigned)t->owner < MAXSPRITES && sprite[t->owner].extra > 0) || t->statnum == STAT_PLAYER)
                    continue;
            }

        // ... since this is not reached:
        if (A_CheckSpriteFlags(t->owner, SFLAG_NOSHADE) || (t->cstat&CSTAT_SPRITE_NOSHADE))
            l = sprite[t->owner].shade;
        else
        {
            if (sector[t->sectnum].ceilingstat&1)
                l = sector[t->sectnum].ceilingshade;
            else
                l = sector[t->sectnum].floorshade;

            if (l < -127)
                l = -127;
        }

        t->shade = l;
    }

    for (j=spritesortcnt-1; j>=0; j--) //Between drawrooms() and drawmasks()
    {
        int32_t switchpic;
        int32_t curframe;
#if !defined LUNATIC
        int32_t scrofs_action;
#else
        int32_t startframe, viewtype;
#endif
        //is the perfect time to animate sprites
        auto const t = &tsprite[j];
        const int32_t i = t->owner;
        // XXX: what's up with the (i < 0) check?
        // NOTE: not const spritetype because set at SET_SPRITE_NOT_TSPRITE (see below).
        EDUKE32_STATIC_ASSERT(sizeof(uspritetype) == sizeof(tspritetype)); // see TSPRITE_SIZE
        auto const pSprite = (i < 0) ? (uspriteptr_t)&tsprite[j] : (uspriteptr_t)&sprite[i];

#ifndef EDUKE32_STANDALONE
        if (adult_lockout && G_CheckAdultTile(DYNAMICTILEMAP(pSprite->picnum)))
        {
            t->xrepeat = t->yrepeat = 0;
            continue;
        }

        if (pSprite->picnum == NATURALLIGHTNING)
        {
            t->shade = -127;
            t->clipdist |= TSPR_FLAGS_NO_SHADOW;
        }
#endif
        if (t->statnum == TSPR_TEMP)
            continue;

        Bassert(i >= 0);

        auto const ps = (pSprite->statnum != STAT_ACTOR && pSprite->picnum == APLAYER && pSprite->owner >= 0) ? g_player[P_GetP(pSprite)].ps : NULL;
        if (ps && ps->newowner == -1)
        {
            t->x -= mulscale16(65536-smoothratio,ps->pos.x-ps->opos.x);
            t->y -= mulscale16(65536-smoothratio,ps->pos.y-ps->opos.y);
            // dirty hack
            if (ps->dead_flag) t->z = ps->opos.z;
            t->z += mulscale16(smoothratio,ps->pos.z-ps->opos.z) -
                (ps->dead_flag ? 0 : PHEIGHT) + PHEIGHT;
        }
        else if (pSprite->picnum != CRANEPOLE)
        {
            t->x -= mulscale16(65536-smoothratio,pSprite->x-actor[i].bpos.x);
            t->y -= mulscale16(65536-smoothratio,pSprite->y-actor[i].bpos.y);
            t->z -= mulscale16(65536-smoothratio,pSprite->z-actor[i].bpos.z);
        }

        const int32_t sect = pSprite->sectnum;

        curframe = AC_CURFRAME(actor[i].t_data);
#if !defined LUNATIC
        scrofs_action = AC_ACTION_ID(actor[i].t_data);
#else
        startframe = actor[i].ac.startframe;
        viewtype = actor[i].ac.viewtype;
#endif
        switchpic = pSprite->picnum;
        // Some special cases because dynamictostatic system can't handle
        // addition to constants.
        if ((pSprite->picnum >= SCRAP6) && (pSprite->picnum<=SCRAP6+7))
            switchpic = SCRAP5;
        else if ((pSprite->picnum==MONEY+1) || (pSprite->picnum==MAIL+1) || (pSprite->picnum==PAPER+1))
            switchpic--;

        switch (DYNAMICTILEMAP(switchpic))
        {
#ifndef EDUKE32_STANDALONE
        case DUKELYINGDEAD__STATIC:
            t->z += (24<<8);
            break;
        case BLOODPOOL__STATIC:
        case FOOTPRINTS__STATIC:
        case FOOTPRINTS2__STATIC:
        case FOOTPRINTS3__STATIC:
        case FOOTPRINTS4__STATIC:
            if (t->pal == 6)
                t->shade = -127;
            fallthrough__;
        case PUKE__STATIC:
        case MONEY__STATIC:
            //case MONEY+1__STATIC:
        case MAIL__STATIC:
            //case MAIL+1__STATIC:
        case PAPER__STATIC:
            //case PAPER+1__STATIC:
            if (adult_lockout && pSprite->pal == 2)
            {
                t->xrepeat = t->yrepeat = 0;
                continue;
            }
            break;
        case TRIPBOMB__STATIC:
            continue;
        case FORCESPHERE__STATIC:
            if (t->statnum == STAT_MISC)
            {
                int16_t const sqa = getangle(sprite[pSprite->owner].x - g_player[screenpeek].ps->pos.x,
                                       sprite[pSprite->owner].y - g_player[screenpeek].ps->pos.y);
                int16_t const sqb = getangle(sprite[pSprite->owner].x - t->x, sprite[pSprite->owner].y - t->y);

                if (klabs(G_GetAngleDelta(sqa,sqb)) > 512)
                    if (ldist(&sprite[pSprite->owner],(spritetype const *)t) < ldist(&sprite[g_player[screenpeek].ps->i],&sprite[pSprite->owner]))
                        t->xrepeat = t->yrepeat = 0;
            }
            continue;
        case BURNING__STATIC:
        case BURNING2__STATIC:
            if (sprite[pSprite->owner].statnum == STAT_PLAYER)
            {
                int const playerNum = P_Get(pSprite->owner);

                if (display_mirror == 0 && playerNum == screenpeek && g_player[playerNum].ps->over_shoulder_on == 0)
                    t->xrepeat = 0;
                else
                {
                    t->ang = getangle(ourx - t->x, oury - t->y);
                    t->x   = sprite[pSprite->owner].x + (sintable[(t->ang + 512) & 2047] >> 10);
                    t->y   = sprite[pSprite->owner].y + (sintable[t->ang & 2047] >> 10);
                }
            }
            break;

        case ATOMICHEALTH__STATIC:
            t->z -= ZOFFSET6;
            break;
        case CRYSTALAMMO__STATIC:
            t->shade = (sintable[((int32_t) totalclock<<4)&2047]>>10);
            continue;
#endif
        case VIEWSCREEN__STATIC:
        case VIEWSCREEN2__STATIC:
        {
            int const viewscrTile = TILE_VIEWSCR;

            if (g_curViewscreen >= 0 && actor[OW(i)].t_data[0] == 1)
            {
                t->picnum = STATIC;
                t->cstat |= (rand()&12);
                t->xrepeat += 10;
                t->yrepeat += 9;
            }
            else if (g_curViewscreen == i && display_mirror != 3 && tileData(viewscrTile))
            {
                t->picnum = viewscrTile;
            }

            break;
        }
#ifndef EDUKE32_STANDALONE
        case SHRINKSPARK__STATIC:
            t->picnum = SHRINKSPARK+(((int32_t) totalclock>>4)&3);
            break;
        case GROWSPARK__STATIC:
            t->picnum = GROWSPARK+(((int32_t) totalclock>>4)&3);
            break;
        case RPG__STATIC:
            if (tilehasmodelorvoxel(t->picnum,t->pal) && !(spriteext[i].flags & SPREXT_NOTMD))
            {
                int32_t v = getangle(t->xvel, t->zvel>>4);

                spriteext[i].pitch = (v > 1023 ? v-2048 : v);
                t->cstat &= ~4;
                break;
            }
            frameOffset = getofs_viewtype_mirrored<7>(t->cstat, pSprite->ang - getangle(pSprite->x-ourx, pSprite->y-oury));
            t->picnum = RPG+frameOffset;
            break;

        case RECON__STATIC:
            if (tilehasmodelorvoxel(t->picnum,t->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
            {
                t->cstat &= ~4;
                break;
            }
            frameOffset = getofs_viewtype_mirrored<7>(t->cstat, pSprite->ang - getangle(pSprite->x-ourx, pSprite->y-oury));

            // RECON_T4
            if (klabs(curframe) > 64)
                frameOffset += 7;  // tilted recon car

            t->picnum = RECON+frameOffset;

            break;
#endif
        case APLAYER__STATIC:
            playerNum = P_GetP(pSprite);

            if (t->pal == 1) t->z -= (18<<8);

            if (g_player[playerNum].ps->over_shoulder_on > 0 && g_player[playerNum].ps->newowner < 0)
            {
                t->ang = fix16_to_int(
                g_player[playerNum].ps->q16ang
                + mulscale16((((g_player[playerNum].ps->q16ang + 1024 - g_player[playerNum].ps->oq16ang) & 2047) - 1024), smoothratio));
                if (tilehasmodelorvoxel(t->picnum, t->pal))
                {
                    static int32_t targetang = 0;

                    if (g_player[playerNum].input->extbits&(1<<1))
                    {
                        if (g_player[playerNum].input->extbits&(1<<2))targetang += 16;
                        else if (g_player[playerNum].input->extbits&(1<<3)) targetang -= 16;
                        else if (targetang > 0) targetang -= targetang>>2;
                        else if (targetang < 0) targetang += (-targetang)>>2;
                    }
                    else
                    {
                        if (g_player[playerNum].input->extbits&(1<<2))targetang -= 16;
                        else if (g_player[playerNum].input->extbits&(1<<3)) targetang += 16;
                        else if (targetang > 0) targetang -= targetang>>2;
                        else if (targetang < 0) targetang += (-targetang)>>2;
                    }

                    targetang = clamp(targetang, -128, 128);
                    t->ang += targetang;
                }
                else if (!display_mirror)
                    t->cstat |= 2;
            }

            if ((g_netServer || ud.multimode > 1) && (display_mirror || screenpeek != playerNum || pSprite->owner == -1))
            {
                if (ud.showweapons && sprite[g_player[playerNum].ps->i].extra > 0 && g_player[playerNum].ps->curr_weapon > 0
                        && spritesortcnt < maxspritesonscreen)
                {
                    auto const newTspr       = &tsprite[spritesortcnt];
                    int const  currentWeapon = g_player[playerNum].ps->curr_weapon;

                    *newTspr         = *t;
                    newTspr->statnum = TSPR_TEMP;
                    newTspr->cstat   = 0;
                    newTspr->pal     = 0;
                    newTspr->picnum  = (currentWeapon == GROW_WEAPON ? GROWSPRITEICON : WeaponPickupSprites[currentWeapon]);
                    newTspr->z       = (pSprite->owner >= 0) ? g_player[playerNum].ps->pos.z - ZOFFSET4 : pSprite->z - (51 << 8);
                    newTspr->xrepeat = (newTspr->picnum == HEAVYHBOMB) ? 10 : 16;
                    newTspr->yrepeat = newTspr->xrepeat;

                    spritesortcnt++;
                }

                if (g_player[playerNum].input->extbits & (1 << 7) && !paused && spritesortcnt < maxspritesonscreen)
                {
                    auto const playerTyping = &tsprite[spritesortcnt];

                    *playerTyping = *t;
                    playerTyping->statnum = TSPR_TEMP;
                    playerTyping->cstat   = 0;
                    playerTyping->picnum  = RESPAWNMARKERGREEN;
                    playerTyping->z       = (pSprite->owner >= 0) ? (g_player[playerNum].ps->pos.z - (20 << 8)) : (pSprite->z - (96 << 8));
                    playerTyping->xrepeat = 32;
                    playerTyping->yrepeat = 32;
                    playerTyping->pal     = 20;

                    spritesortcnt++;
                }
            }

            if (pSprite->owner == -1)
            {
                if (tilehasmodelorvoxel(pSprite->picnum,t->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
                {
                    frameOffset = 0;
                    t->cstat &= ~4;
                }
                else
                    frameOffset = getofs_viewtype_mirrored<5>(t->cstat, pSprite->ang - oura);

                if (sector[pSprite->sectnum].lotag == ST_2_UNDERWATER) frameOffset += 1795-1405;
                else if ((actor[i].floorz-pSprite->z) > (64<<8)) frameOffset += 60;

                t->picnum += frameOffset;
                t->pal = g_player[playerNum].ps->palookup;

                goto PALONLY;
            }

            if (g_player[playerNum].ps->on_crane == -1 && (sector[pSprite->sectnum].lotag&0x7ff) != 1)  // ST_1_ABOVE_WATER ?
            {
                l = pSprite->z-actor[g_player[playerNum].ps->i].floorz+(3<<8);
                // SET_SPRITE_NOT_TSPRITE
                if (l > 1024 && pSprite->yrepeat > 32 && pSprite->extra > 0)
                    t->yoffset = (int8_t)tabledivide32_noinline(l, pSprite->yrepeat<<2);
                else t->yoffset=0;
            }

#ifndef EDUKE32_STANDALONE
            if (!FURY && g_player[playerNum].ps->newowner > -1)
            {
                // Display APLAYER sprites with action PSTAND when viewed through
                // a camera.  Not implemented for Lunatic.
#if !defined LUNATIC
                const intptr_t *aplayer_scr = g_tile[APLAYER].execPtr;
                // [0]=strength, [1]=actionofs, [2]=moveofs

                scrofs_action = aplayer_scr[1];
#endif
                curframe = 0;
            }
#endif
            if (ud.camerasprite == -1 && g_player[playerNum].ps->newowner == -1)
            {
                if (pSprite->owner >= 0 && display_mirror == 0 && g_player[playerNum].ps->over_shoulder_on == 0)
                {
                    if ((!g_netServer && ud.multimode < 2) || ((g_netServer || ud.multimode > 1) && playerNum == screenpeek))
                    {
                        if (videoGetRenderMode() == REND_POLYMER)
                            t->clipdist |= TSPR_FLAGS_INVISIBLE_WITH_SHADOW;
                        else
                        {
                            t->owner = -1;
                            t->xrepeat = t->yrepeat = 0;
                            continue;
                        }

                        if (tilehasmodelorvoxel(pSprite->picnum, t->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
                        {
                            frameOffset = 0;
                            t->cstat &= ~4;
                        }
                        else
                            frameOffset = getofs_viewtype_mirrored<5>(t->cstat, pSprite->ang - oura);

                        if (sector[t->sectnum].lotag == ST_2_UNDERWATER) frameOffset += 1795-1405;
                        else if ((actor[i].floorz-pSprite->z) > (64<<8)) frameOffset += 60;

                        t->picnum += frameOffset;
                        t->pal = g_player[playerNum].ps->palookup;
                    }
                }
            }
PALONLY:
            G_MaybeTakeOnFloorPal(t, sect);

            if (pSprite->owner == -1) continue;

            if (t->z > actor[i].floorz && t->xrepeat < 32)
                t->z = actor[i].floorz;

            break;
#ifndef EDUKE32_STANDALONE
        case JIBS1__STATIC:
        case JIBS2__STATIC:
        case JIBS3__STATIC:
        case JIBS4__STATIC:
        case JIBS5__STATIC:
        case JIBS6__STATIC:
        case HEADJIB1__STATIC:
        case LEGJIB1__STATIC:
        case ARMJIB1__STATIC:
        case LIZMANHEAD1__STATIC:
        case LIZMANARM1__STATIC:
        case LIZMANLEG1__STATIC:
        case DUKELEG__STATIC:
        case DUKEGUN__STATIC:
        case DUKETORSO__STATIC:
            if (adult_lockout)
            {
                t->xrepeat = t->yrepeat = 0;
                continue;
            }
            if (t->pal == 6)
                t->shade = -120;
            fallthrough__;
        case SCRAP1__STATIC:
        case SCRAP2__STATIC:
        case SCRAP3__STATIC:
        case SCRAP4__STATIC:
        case SCRAP5__STATIC:
            if (actor[i].picnum == BLIMP && t->picnum == SCRAP1 && pSprite->yvel >= 0)
                t->picnum = pSprite->yvel < MAXUSERTILES ? pSprite->yvel : 0;
            else t->picnum += T1(i);
            t->shade = -128+6 < t->shade ? t->shade-6 : -128; // effectively max(t->shade-6, -128) while avoiding (signed!) underflow

            G_MaybeTakeOnFloorPal(t, sect);
            break;
        case WATERBUBBLE__STATIC:
            if (sector[t->sectnum].floorpicnum == FLOORSLIME)
            {
                t->pal = 7;
                break;
            }
            fallthrough__;
#endif
        default:
            G_MaybeTakeOnFloorPal(t, sect);
            break;
        }

        if (G_TileHasActor(pSprite->picnum))
        {
#if !defined LUNATIC
            if ((unsigned)scrofs_action + ACTION_PARAM_COUNT > (unsigned)g_scriptSize)
                goto skip;

            int32_t viewtype = apScript[scrofs_action + ACTION_VIEWTYPE];
            uint16_t const action_flags = apScript[scrofs_action + ACTION_FLAGS];
#else
            uint16_t const action_flags = actor[i].ac.flags;
#endif

            int const invertp = viewtype < 0;
            l = klabs(viewtype);

            if (tilehasmodelorvoxel(pSprite->picnum,t->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
            {
                frameOffset = 0;
                t->cstat &= ~4;
            }
            else
            {
                int const viewAng = ((l > 4 && l != 8) || action_flags & AF_VIEWPOINT) ? getangle(pSprite->x-ourx, pSprite->y-oury) : oura;
                int const angDiff = invertp ? viewAng - pSprite->ang : pSprite->ang - viewAng;

                switch (l)
                {
                case 2:
                    frameOffset = getofs_viewtype<8>(angDiff) & 1;
                    break;

                case 3:
                case 4:
                    frameOffset = viewtype_mirror<7>(t->cstat, getofs_viewtype<16>(angDiff) & 7);
                    break;

                case 5:
                    frameOffset = getofs_viewtype_mirrored<5>(t->cstat, angDiff);
                    break;
                case 7:
                    frameOffset = getofs_viewtype_mirrored<7>(t->cstat, angDiff);
                    break;
                case 8:
                    frameOffset = getofs_viewtype<8>(angDiff);
                    t->cstat &= ~4;
                    break;
                case 9:
                    frameOffset = getofs_viewtype_mirrored<9>(t->cstat, angDiff);
                    break;
                case 12:
                    frameOffset = getofs_viewtype<12>(angDiff);
                    t->cstat &= ~4;
                    break;
                case 16:
                    frameOffset = getofs_viewtype<16>(angDiff);
                    t->cstat &= ~4;
                    break;
                default:
                    frameOffset = 0;
                    break;
                }
            }

#if !defined LUNATIC
            t->picnum += frameOffset + apScript[scrofs_action + ACTION_STARTFRAME] + viewtype*curframe;
#else
            t->picnum += frameOffset + startframe + viewtype*curframe;
#endif
            // XXX: t->picnum can be out-of-bounds by bad user code.

            if (viewtype > 0)
                while (tilesiz[t->picnum].x == 0 && t->picnum > 0)
                    t->picnum -= l;       //Hack, for actors

            if (actor[i].dispicnum >= 0)
                actor[i].dispicnum = t->picnum;
        }
//        else if (display_mirror == 1)
//            t->cstat |= 4;
        /* completemirror() already reverses the drawn frame, so the above isn't necessary.
         * Even Polymost's and Polymer's mirror seems to function correctly this way. */

#if !defined LUNATIC
skip:
#endif
        // Night vision goggles tsprite tinting.
        // XXX: Currently, for the splitscreen mod, sprites will be pal6-colored iff the first
        // player has nightvision on.  We should pass stuff like "from which player is this view
        // supposed to be" as parameters ("drawing context") instead of relying on globals.
        if (g_player[screenpeek].ps->inv_amount[GET_HEATS] > 0 && g_player[screenpeek].ps->heat_on &&
                (A_CheckEnemySprite(pSprite) || A_CheckSpriteFlags(t->owner,SFLAG_NVG) || pSprite->picnum == APLAYER || pSprite->statnum == STAT_DUMMYPLAYER))
        {
            t->pal = 6;
            t->shade = 0;
        }

        // Fake floor shadow, implemented by inserting a new tsprite.
        if (pSprite->statnum == STAT_DUMMYPLAYER || A_CheckEnemySprite(pSprite) || A_CheckSpriteFlags(t->owner,SFLAG_SHADOW) || (pSprite->picnum == APLAYER && pSprite->owner >= 0))
            if (t->statnum != TSPR_TEMP && pSprite->picnum != EXPLOSION2 && pSprite->picnum != HANGLIGHT && pSprite->picnum != DOMELITE && pSprite->picnum != HOTMEAT)
            {
                if (actor[i].dispicnum < 0)
                {
#ifdef DEBUGGINGAIDS
                    // A negative actor[i].dispicnum used to mean 'no floor shadow please', but
                    // that was a bad hack since the value could propagate to sprite[].picnum.
                    Printf(TEXTCOLOR_RED "actor[%d].dispicnum = %d\n", i, actor[i].dispicnum);
#endif
                    actor[i].dispicnum=0;
                    continue;
                }

                if (actor[i].flags & SFLAG_NOFLOORSHADOW)
                    continue;

                if (r_shadows && spritesortcnt < (maxspritesonscreen-2)
#ifdef POLYMER
                    && !(videoGetRenderMode() == REND_POLYMER && pr_lighting != 0)
#endif
                    )
                {
                    int const shadowZ = ((sector[sect].lotag & 0xff) > 2 || pSprite->statnum == STAT_PROJECTILE ||
                                   pSprite->statnum == STAT_MISC || pSprite->picnum == DRONE || pSprite->picnum == COMMANDER)
                                  ? sector[sect].floorz
                                  : actor[i].floorz;

                    if ((pSprite->z-shadowZ) < ZOFFSET3 && g_player[screenpeek].ps->pos.z < shadowZ)
                    {
                        tspriteptr_t tsprShadow = &tsprite[spritesortcnt];

                        *tsprShadow         = *t;
                        tsprShadow->statnum = TSPR_TEMP;
                        tsprShadow->yrepeat = (t->yrepeat >> 3);

                        if (t->yrepeat < 4)
                            t->yrepeat = 4;

                        tsprShadow->shade   = 127;
                        tsprShadow->cstat  |= 2;
                        tsprShadow->z       = shadowZ;
                        tsprShadow->pal     = ud.shadow_pal;

#ifdef USE_OPENGL
                        if (videoGetRenderMode() >= REND_POLYMOST)
                        {
                            if (tilehasmodelorvoxel(t->picnum,t->pal))
                            {
                                tsprShadow->yrepeat = 0;
                                // 512:trans reverse
                                //1024:tell MD2SPRITE.C to use Z-buffer hacks to hide overdraw issues
                                tsprShadow->clipdist |= TSPR_FLAGS_MDHACK;
                                tsprShadow->cstat |= 512;
                            }
                            else
                            {
                                int const camang = display_mirror ? ((2048 - fix16_to_int(CAMERA(q16ang))) & 2047) : fix16_to_int(CAMERA(q16ang));
                                vec2_t const ofs = { sintable[(camang+512)&2047]>>11, sintable[(camang)&2047]>>11};

                                tsprShadow->x += ofs.x;
                                tsprShadow->y += ofs.y;
                            }
                        }
#endif
                        spritesortcnt++;
                    }
                }
            }

#ifdef LUNATIC
        bool const haveAction = false; // FIXME!
#else
        bool const haveAction = scrofs_action != 0 && (unsigned)scrofs_action + ACTION_PARAM_COUNT <= (unsigned)g_scriptSize;
#endif

        switch (DYNAMICTILEMAP(pSprite->picnum))
        {
#ifndef EDUKE32_STANDALONE
        case LASERLINE__STATIC:
            if (sector[t->sectnum].lotag == ST_2_UNDERWATER) t->pal = 8;
            t->z = sprite[pSprite->owner].z-(3<<8);
            if (g_tripbombLaserMode == 2 && g_player[screenpeek].ps->heat_on == 0)
                t->yrepeat = 0;
            fallthrough__;
        case EXPLOSION2BOT__STATIC:
        case FREEZEBLAST__STATIC:
        case ATOMICHEALTH__STATIC:
        case FIRELASER__STATIC:
        case SHRINKSPARK__STATIC:
        case GROWSPARK__STATIC:
        case CHAINGUN__STATIC:
        case SHRINKEREXPLOSION__STATIC:
        case RPG__STATIC:
        case FLOORFLAME__STATIC:
#endif
        case EXPLOSION2__STATIC:
            if (t->picnum == EXPLOSION2)
            {
                g_player[screenpeek].ps->visibility = -127;
                //g_restorePalette = 1;   // JBF 20040101: why?
            }
            t->shade = -127;
            t->clipdist |= TSPR_FLAGS_DRAW_LAST | TSPR_FLAGS_NO_SHADOW;
            break;
#ifndef EDUKE32_STANDALONE
        case FIRE__STATIC:
        case FIRE2__STATIC:
            t->cstat |= 128;
            fallthrough__;
        case BURNING__STATIC:
        case BURNING2__STATIC:
            if (sprite[pSprite->owner].picnum != TREE1 && sprite[pSprite->owner].picnum != TREE2)
                t->z = actor[t->owner].floorz;
            t->shade = -127;
            fallthrough__;
        case SMALLSMOKE__STATIC:
            t->clipdist |= TSPR_FLAGS_DRAW_LAST | TSPR_FLAGS_NO_SHADOW;
            break;
        case COOLEXPLOSION1__STATIC:
            t->shade = -127;
            t->clipdist |= TSPR_FLAGS_DRAW_LAST | TSPR_FLAGS_NO_SHADOW;
            t->picnum += (pSprite->shade>>1);
            break;
        case PLAYERONWATER__STATIC:
            t->shade = sprite[pSprite->owner].shade;
            if (haveAction)
                break;
            if (tilehasmodelorvoxel(pSprite->picnum,pSprite->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
            {
                frameOffset = 0;
                t->cstat &= ~4;
            }
            else
                frameOffset = getofs_viewtype_mirrored<5>(t->cstat, t->ang - oura);

            t->picnum = pSprite->picnum+frameOffset+((T1(i)<4)*5);
            break;

        case WATERSPLASH2__STATIC:
            // WATERSPLASH_T2
            t->picnum = WATERSPLASH2+T2(i);
            break;
        case SHELL__STATIC:
            t->picnum = pSprite->picnum+(T1(i)&1);
            fallthrough__;
        case SHOTGUNSHELL__STATIC:
            t->cstat |= 12;
            if (T1(i) > 2) t->cstat &= ~16;
            else if (T1(i) > 1) t->cstat &= ~4;
            break;
        case FRAMEEFFECT1_13__STATIC:
            if (PLUTOPAK) break;
            fallthrough__;
#endif
        case FRAMEEFFECT1__STATIC:
            if (pSprite->owner >= 0 && sprite[pSprite->owner].statnum < MAXSTATUS)
            {
                if (sprite[pSprite->owner].picnum == APLAYER)
                    if (ud.camerasprite == -1)
                        if (screenpeek == P_Get(pSprite->owner) && display_mirror == 0)
                        {
                            t->owner = -1;
                            break;
                        }
                if ((sprite[pSprite->owner].cstat&32768) == 0)
                {
                    if (!actor[pSprite->owner].dispicnum)
                        t->picnum = actor[i].t_data[1];
                    else t->picnum = actor[pSprite->owner].dispicnum;

                    if (!G_MaybeTakeOnFloorPal(t, sect))
                        t->pal = sprite[pSprite->owner].pal;

                    t->shade = sprite[pSprite->owner].shade;
                    t->ang = sprite[pSprite->owner].ang;
                    t->cstat = 2|sprite[pSprite->owner].cstat;
                }
            }
            break;

        case CAMERA1__STATIC:
        case RAT__STATIC:
            if (haveAction)
                break;
            if (tilehasmodelorvoxel(pSprite->picnum,pSprite->pal) && !(spriteext[i].flags&SPREXT_NOTMD))
            {
                t->cstat &= ~4;
                break;
            }
            frameOffset = getofs_viewtype_mirrored<5>(t->cstat, t->ang - oura);
            t->picnum = pSprite->picnum+frameOffset;
            break;
        }

        actor[i].dispicnum = t->picnum;
#if 0
        // why?
        if (sector[t->sectnum].floorpicnum == MIRROR)
            t->xrepeat = t->yrepeat = 0;
#endif
    }

    if (VM_HaveEvent(EVENT_ANIMATESPRITES))
    {
        for (j = spritesortcnt-1; j>=0; j--)
            G_DoEventAnimSprites(j);
    }

#ifdef LUNATIC
    VM_OnEvent(EVENT_ANIMATEALLSPRITES);
#endif
#ifdef DEBUGGINGAIDS
    g_spriteStat.numonscreen = spritesortcnt;
#endif
}

void G_InitTimer(int32_t ticspersec)
{
    if (g_timerTicsPerSecond != ticspersec)
    {
        timerUninit();
        timerInit(ticspersec);
        g_timerTicsPerSecond = ticspersec;
    }
}


static int32_t g_RTSPlaying;

// Returns: started playing?
int G_StartRTS(int lumpNum, int localPlayer)
{
    if (!adult_lockout && SoundEnabled() &&
        RTS_IsInitialized() && g_RTSPlaying == 0 && (snd_speech & (localPlayer ? 1 : 4)))
    {
        auto sid = RTS_GetSoundID(lumpNum - 1);
        if (sid != -1)
        {
            S_PlaySound(sid, CHAN_AUTO, CHANF_UI);
            g_RTSPlaying = 7;
            return 1;
        }
    }

    return 0;
}


// Trying to sanitize the mess of options and the mess of variables the mess was stored in. (Did I say this was a total mess before...? >) )
// Hopefully this is more comprehensible, at least it neatly stores everything useful in a single linear value...
bool GameInterface::validate_hud(int layout)
{
	if (layout <= 6)	// Status bar with border
	{
		return !(ud.statusbarflags & STATUSBAR_NOSHRINK);
	}
	else if (layout == 7) // Status bar fullscreen
	{
		return (!(ud.statusbarflags & STATUSBAR_NOFULL) || !(ud.statusbarflags & STATUSBAR_NOOVERLAY));
	}
	else if (layout == 8)	// Status bar overlay
	{
		return !(ud.statusbarflags & STATUSBAR_NOOVERLAY);
	}
	else if (layout == 9)	// Fullscreen HUD
	{
		return (!(ud.statusbarflags & STATUSBAR_NOMINI) || !(ud.statusbarflags & STATUSBAR_NOMODERN));
	}
	else if (layout == 10)
	{
		return !(ud.statusbarflags & STATUSBAR_NOMODERN);
	}
	else if (layout == 11)
	{
		return !(ud.statusbarflags & STATUSBAR_NONONE);
	}
	return false;
}

void GameInterface::set_hud_layout(int layout)
{
	static const uint8_t screen_size_vals[] = { 60, 54, 48, 40, 32, 24, 16, 8, 8, 4, 4, 0 };
	if (validate_hud(layout))
	{
		ud.screen_size = screen_size_vals[layout];
		ud.statusbarmode = layout >= 8;
		ud.althud = layout >= 10;
		G_UpdateScreenArea();
	}
}

void GameInterface::set_hud_scale(int scale)
{
    ud.statusbarscale = clamp(scale, 36, 100);
    G_UpdateScreenArea();
}

void G_HandleLocalKeys(void)
{
//    CONTROL_ProcessBinds();
    auto &myplayer = *g_player[myconnectindex].ps;

    if (ud.recstat == 2)
    {
        ControlInfo noshareinfo;
        CONTROL_GetInput(&noshareinfo);
    }

    if (g_player[myconnectindex].gotvote == 0 && voting != -1 && voting != myconnectindex)
    {
        if (inputState.UnboundKeyPressed(sc_F1) || inputState.UnboundKeyPressed(sc_F2) || cl_autovote)
        {
            G_AddUserQuote(GStrings("VoteCast"));
            Net_SendMapVote(inputState.UnboundKeyPressed(sc_F1) || cl_autovote ? cl_autovote-1 : 0);
            inputState.ClearKeyStatus(sc_F1);
            inputState.ClearKeyStatus(sc_F2);
        }
    }

    if (!ALT_IS_PRESSED && ud.overhead_on == 0 && (myplayer.gm & MODE_TYPE) == 0)
    {
        if (buttonMap.ButtonDown(gamefunc_Enlarge_Screen))
        {
            buttonMap.ClearButton(gamefunc_Enlarge_Screen);

            if (!SHIFTS_IS_PRESSED)
            {
				if (G_ChangeHudLayout(1))
				{
					S_PlaySound(THUD, CHAN_AUTO, CHANF_UI);
				}
            }
            else
            {
                hud_scale = hud_scale + 5;
            }

            G_UpdateScreenArea();
        }

        if (buttonMap.ButtonDown(gamefunc_Shrink_Screen))
        {
            buttonMap.ClearButton(gamefunc_Shrink_Screen);

            if (!SHIFTS_IS_PRESSED)
            {
				if (G_ChangeHudLayout(-1))
				{
					S_PlaySound(THUD, CHAN_AUTO, CHANF_UI);
				}
            }
            else
            {
                hud_scale = hud_scale - 5;
            }

            G_UpdateScreenArea();
        }
    }

    if (myplayer.cheat_phase == 1 || (myplayer.gm & (MODE_MENU|MODE_TYPE)))
        return;

    if (buttonMap.ButtonDown(gamefunc_See_Coop_View) && (GTFLAGS(GAMETYPE_COOPVIEW) || ud.recstat == 2))
    {
        buttonMap.ClearButton(gamefunc_See_Coop_View);
        screenpeek = connectpoint2[screenpeek];
        if (screenpeek == -1) screenpeek = 0;
        g_restorePalette = -1;
    }

    if ((g_netServer || ud.multimode > 1) && buttonMap.ButtonDown(gamefunc_Show_Opponents_Weapon))
    {
        buttonMap.ClearButton(gamefunc_Show_Opponents_Weapon);
        ud.config.ShowWeapons = ud.showweapons = 1-ud.showweapons;
        P_DoQuote(QUOTE_WEAPON_MODE_OFF-ud.showweapons, &myplayer);
    }

    if (buttonMap.ButtonDown(gamefunc_Toggle_Crosshair))
    {
        buttonMap.ClearButton(gamefunc_Toggle_Crosshair);
		cl_crosshair = !cl_crosshair;
        P_DoQuote(QUOTE_CROSSHAIR_OFF-cl_crosshair, &myplayer);
    }

    if (ud.overhead_on && buttonMap.ButtonDown(gamefunc_Map_Follow_Mode))
    {
        buttonMap.ClearButton(gamefunc_Map_Follow_Mode);
        ud.scrollmode = 1-ud.scrollmode;
        if (ud.scrollmode)
        {
            ud.folx = g_player[screenpeek].ps->opos.x;
            ud.foly = g_player[screenpeek].ps->opos.y;
            ud.fola = fix16_to_int(g_player[screenpeek].ps->oq16ang);
        }
        P_DoQuote(QUOTE_MAP_FOLLOW_OFF+ud.scrollmode, &myplayer);
    }

    if (inputState.UnboundKeyPressed(sc_ScrollLock))
    {
        inputState.ClearKeyStatus(sc_ScrollLock);

        switch (ud.recstat)
        {
        case 0:
            if (SHIFTS_IS_PRESSED)
                G_OpenDemoWrite();
            break;
        case 1:
            G_CloseDemoWrite();
            break;
        }
    }

    if (ud.recstat == 2)
    {
        if (inputState.GetKeyStatus(sc_Space))
        {
            inputState.ClearKeyStatus(sc_Space);

            g_demo_paused = !g_demo_paused;
            g_demo_rewind = 0;

            if (g_demo_paused)
                FX_StopAllSounds();
        }

        if (inputState.GetKeyStatus(sc_Tab))
        {
            inputState.ClearKeyStatus(sc_Tab);
            g_demo_showStats = !g_demo_showStats;
        }

#if 0
        if (inputState.GetKeyStatus(sc_kpad_Plus))
        {
            G_InitTimer(240);
        }
        else if (inputState.GetKeyStatus(sc_kpad_Minus))
        {
            G_InitTimer(60);
        }
        else if (g_timerTicsPerSecond != 120)
        {
            G_InitTimer(120);
        }
#endif

        if (inputState.GetKeyStatus(sc_kpad_6))
        {
            inputState.ClearKeyStatus(sc_kpad_6);

            int const fwdTics = (15 << (int)ALT_IS_PRESSED) << (2 * (int)SHIFTS_IS_PRESSED);
            g_demo_goalCnt    = g_demo_paused ? g_demo_cnt + 1 : g_demo_cnt + REALGAMETICSPERSEC * fwdTics;
            g_demo_rewind     = 0;

            if (g_demo_goalCnt > g_demo_totalCnt)
                g_demo_goalCnt = 0;
            else
                Demo_PrepareWarp();
        }
        else if (inputState.GetKeyStatus(sc_kpad_4))
        {
            inputState.ClearKeyStatus(sc_kpad_4);

            int const rewindTics = (15 << (int)ALT_IS_PRESSED) << (2 * (int)SHIFTS_IS_PRESSED);
            g_demo_goalCnt       = g_demo_paused ? g_demo_cnt - 1 : g_demo_cnt - REALGAMETICSPERSEC * rewindTics;
            g_demo_rewind        = 1;

            if (g_demo_goalCnt <= 0)
                g_demo_goalCnt = 1;

            Demo_PrepareWarp();
        }

    }

    if (SHIFTS_IS_PRESSED || ALT_IS_PRESSED || WIN_IS_PRESSED)
    {
        int ridiculeNum = 0;

        // NOTE: sc_F1 .. sc_F10 are contiguous. sc_F11 is not sc_F10+1.
        for (bssize_t j=sc_F1; j<=sc_F10; j++)
            if (inputState.UnboundKeyPressed(j))
            {
                inputState.ClearKeyStatus(j);
                ridiculeNum = j - sc_F1 + 1;
                break;
            }

        if (ridiculeNum)
        {
            if (SHIFTS_IS_PRESSED)
            {
                G_AddUserQuote(*CombatMacros[ridiculeNum-1]);
				Net_SendTaunt(ridiculeNum);

                pus = NUMPAGES;
                pub = NUMPAGES;

                return;
            }

            // Not SHIFT -- that is, either some ALT or WIN.
            if (G_StartRTS(ridiculeNum, 1))
            {
				Net_SendRTS(ridiculeNum);
                pus = NUMPAGES;
                pub = NUMPAGES;

                return;
            }
        }
    }
    else
    {
        if (buttonMap.ButtonDown(gamefunc_Third_Person_View))
        {
            buttonMap.ClearButton(gamefunc_Third_Person_View);

            myplayer.over_shoulder_on = !myplayer.over_shoulder_on;

            CAMERADIST  = 0;
            CAMERACLOCK = (int32_t) totalclock;

            P_DoQuote(QUOTE_VIEW_MODE_OFF + myplayer.over_shoulder_on, &myplayer);
        }


        if (ud.overhead_on != 0)
        {
            int const timerOffset = ((int) totalclock - nonsharedtimer);
            nonsharedtimer += timerOffset;

            if (buttonMap.ButtonDown(gamefunc_Enlarge_Screen))
                myplayer.zoom += mulscale6(timerOffset, max<int>(myplayer.zoom, 256));

            if (buttonMap.ButtonDown(gamefunc_Shrink_Screen))
                myplayer.zoom -= mulscale6(timerOffset, max<int>(myplayer.zoom, 256));

            myplayer.zoom = clamp(myplayer.zoom, 48, 2048);
        }
    }

#if 0 // fixme: We should not query Esc here, this needs to be done differently
    if (I_EscapeTrigger() && ud.overhead_on && myplayer.newowner == -1)
    {
        I_EscapeTriggerClear();
        ud.last_overhead = ud.overhead_on;
        ud.overhead_on   = 0;
        ud.scrollmode    = 0;
        G_UpdateScreenArea();
    }
#endif

    if (buttonMap.ButtonDown(gamefunc_Map))
    {
        buttonMap.ClearButton(gamefunc_Map);
        if (ud.last_overhead != ud.overhead_on && ud.last_overhead)
        {
            ud.overhead_on = ud.last_overhead;
            ud.last_overhead = 0;
        }
        else
        {
            ud.overhead_on++;
            if (ud.overhead_on == 3) ud.overhead_on = 0;
            ud.last_overhead = ud.overhead_on;
        }

        g_restorePalette = 1;
        G_UpdateScreenArea();
    }
}

// Returns:
//   0: all OK
//  -1: ID declaration was invalid:
static int32_t S_DefineMusic(const char *ID, const char *name)
{
    int32_t sel = MUS_FIRST_SPECIAL;

    Bassert(ID != NULL);

    if (!Bstrcmp(ID,"intro"))
    {
        // nothing
    }
    else if (!Bstrcmp(ID,"briefing"))
    {
        sel++;
    }
    else if (!Bstrcmp(ID,"loading"))
    {
        sel += 2;
    }
    else if (!Bstrcmp(ID,"usermap"))
    {
        sel += 3;
    }
    else
    {
        sel = G_GetMusicIdx(ID);
        if (sel < 0)
            return -1;
    }

    mapList[sel].music = name;
    return 0;
}

static int parsedefinitions_game(scriptfile *, int);

static void parsedefinitions_game_include(const char *fileName, scriptfile *pScript, const char *cmdtokptr, int const firstPass)
{
    scriptfile *included = scriptfile_fromfile(fileName);

    if (!included)
    {
        if (!Bstrcasecmp(cmdtokptr,"null") || pScript == NULL) // this is a bit overboard to prevent unused parameter warnings
            {
           // Printf("Warning: Failed including %s as module\n", fn);
            }
/*
        else
            {
            Printf("Warning: Failed including %s on line %s:%d\n",
                       fn, script->filename,scriptfile_getlinum(script,cmdtokptr));
            }
*/
    }
    else
    {
        parsedefinitions_game(included, firstPass);
        scriptfile_close(included);
    }
}

static void parsedefinitions_game_animsounds(scriptfile *pScript, const char * blockEnd, char const * fileName, dukeanim_t * animPtr)
{
    size_t numPairs = 0;

    animPtr->Sounds.Clear();

    int defError = 1;
    uint16_t lastFrameNum = 1;

    while (pScript->textptr < blockEnd)
    {
        int32_t frameNum;
        int32_t soundNum;

        // HACK: we've reached the end of the list
        //  (hack because it relies on knowledge of
        //   how scriptfile_* preprocesses the text)
        if (blockEnd - pScript->textptr == 1)
            break;

        // would produce error when it encounters the closing '}'
        // without the above hack
        if (scriptfile_getnumber(pScript, &frameNum))
            break;

        defError = 1;

        if (scriptfile_getsymbol(pScript, &soundNum))
            break;

        // frame numbers start at 1 for us
        if (frameNum <= 0)
        {
            Printf("Error: frame number must be greater zero on line %s:%d\n", pScript->filename,
                       scriptfile_getlinum(pScript, pScript->ltextptr));
            break;
        }

        if (frameNum < lastFrameNum)
        {
            Printf("Error: frame numbers must be in (not necessarily strictly)"
                       " ascending order (line %s:%d)\n",
                       pScript->filename, scriptfile_getlinum(pScript, pScript->ltextptr));
            break;
        }

        lastFrameNum = frameNum;

        if ((unsigned)soundNum >= MAXSOUNDS && soundNum != -1)
        {
            Printf("Error: sound number #%d invalid on line %s:%d\n", soundNum, pScript->filename,
                       scriptfile_getlinum(pScript, pScript->ltextptr));
            break;
        }


        defError = 0;

        animsound_t sound;
        sound.frame = frameNum;
        sound.sound = soundNum;
        animPtr->Sounds.Push(sound);

        ++numPairs;
    }

    if (!defError)
    {
        // Printf("Defined sound sequence for hi-anim \"%s\" with %d frame/sound pairs\n",
        //           hardcoded_anim_tokens[animnum].text, numpairs);
    }
    else
    {
        Printf("Failed defining sound sequence for anim \"%s\".\n", fileName);
    }
    animPtr->Sounds.ShrinkToFit();
}

static int parsedefinitions_game(scriptfile *pScript, int firstPass)
{
    int   token;
    char *pToken;

    static const tokenlist tokens[] =
    {
        { "include",         T_INCLUDE          },
        { "#include",        T_INCLUDE          },
        { "includedefault",  T_INCLUDEDEFAULT   },
        { "#includedefault", T_INCLUDEDEFAULT   },
        { "loadgrp",         T_LOADGRP          },
        { "cachesize",       T_CACHESIZE        },
        { "noautoload",      T_NOAUTOLOAD       },
        { "music",           T_MUSIC            },
        { "sound",           T_SOUND            },
        { "cutscene",        T_CUTSCENE         },
        { "animsounds",      T_ANIMSOUNDS       },
        { "renamefile",      T_RENAMEFILE       },
        { "globalgameflags", T_GLOBALGAMEFLAGS  },
        { "newgamechoices",  T_NEWGAMECHOICES   },
    };

    static const tokenlist soundTokens[] =
    {
        { "id",       T_ID },
        { "file",     T_FILE },
        { "minpitch", T_MINPITCH },
        { "maxpitch", T_MAXPITCH },
        { "priority", T_PRIORITY },
        { "type",     T_TYPE },
        { "distance", T_DISTANCE },
        { "volume",   T_VOLUME },
    };

    static const tokenlist animTokens [] =
    {
        { "delay",         T_DELAY },
        { "aspect",        T_ASPECT },
        { "sounds",        T_SOUND },
        { "forcefilter",   T_FORCEFILTER },
        { "forcenofilter", T_FORCENOFILTER },
        { "texturefilter", T_TEXTUREFILTER },
    };

    static const tokenlist newGameTokens[] =
    {
        { "choice",        T_CHOICE },
    };
    static const tokenlist newGameChoiceTokens[] =
    {
        { "name",          T_NAME },
        { "locked",        T_LOCKED },
        { "hidden",        T_HIDDEN },
        { "choice",        T_CHOICE },
        { "usercontent",   T_USERCONTENT },
    };
    static const tokenlist newGameSubchoiceTokens[] =
    {
        { "name",          T_NAME },
        { "locked",        T_LOCKED },
        { "hidden",        T_HIDDEN },
    };

    do
    {
        token  = getatoken(pScript, tokens, ARRAY_SIZE(tokens));
        pToken = pScript->ltextptr;

        switch (token)
        {
        case T_LOADGRP:
        {
            char *fileName;

            if (!scriptfile_getstring(pScript,&fileName) && firstPass)
            {
				fileSystem.AddAdditionalFile(fileName);
            }
        }
        break;
        case T_CACHESIZE:
        {
            int32_t cacheSize;

            if (scriptfile_getnumber(pScript, &cacheSize) || !firstPass)
                break;
        }
        break;
        case T_INCLUDE:
        {
            char *fileName;

            if (!scriptfile_getstring(pScript, &fileName))
                parsedefinitions_game_include(fileName, pScript, pToken, firstPass);

            break;
        }
        case T_INCLUDEDEFAULT:
        {
            parsedefinitions_game_include(G_DefaultDefFile(), pScript, pToken, firstPass);
            break;
        }
        case T_NOAUTOLOAD:
            if (firstPass)
                gNoAutoLoad = 1;
            break;
        case T_MUSIC:
        {
            char *tokenPtr = pScript->ltextptr;
            char *musicID  = NULL;
            char *fileName = NULL;
            char *musicEnd;

            if (scriptfile_getbraces(pScript, &musicEnd))
                break;

            while (pScript->textptr < musicEnd)
            {
                switch (getatoken(pScript, soundTokens, ARRAY_SIZE(soundTokens)))
                {
                    case T_ID: scriptfile_getstring(pScript, &musicID); break;
                    case T_FILE: scriptfile_getstring(pScript, &fileName); break;
                }
            }

            if (!firstPass)
            {
                if (musicID==NULL)
                {
                    Printf("Error: missing ID for music definition near line %s:%d\n",
                               pScript->filename, scriptfile_getlinum(pScript,tokenPtr));
                    break;
                }

                if (fileName == NULL || fileSystem.FileExists(fileName))
                    break;

                if (S_DefineMusic(musicID, fileName) == -1)
                    Printf("Error: invalid music ID on line %s:%d\n", pScript->filename, scriptfile_getlinum(pScript, tokenPtr));
            }
        }
        break;

        case T_CUTSCENE:
        {
            char *fileName = NULL;

            scriptfile_getstring(pScript, &fileName);

            char *animEnd;

            if (scriptfile_getbraces(pScript, &animEnd))
                break;

            if (!firstPass)
            {
                dukeanim_t *animPtr = Anim_Find(fileName);

                if (!animPtr)
                {
                    animPtr = Anim_Create(fileName);
                    animPtr->framedelay = 10;
                    animPtr->frameflags = 0;
                }

                int32_t temp;

                while (pScript->textptr < animEnd)
                {
                    switch (getatoken(pScript, animTokens, ARRAY_SIZE(animTokens)))
                    {
                        case T_DELAY:
                            scriptfile_getnumber(pScript, &temp);
                            animPtr->framedelay = temp;
                            break;
                        case T_ASPECT:
                        {
                            double dtemp, dtemp2;
                            scriptfile_getdouble(pScript, &dtemp);
                            scriptfile_getdouble(pScript, &dtemp2);
                            animPtr->frameaspect1 = dtemp;
                            animPtr->frameaspect2 = dtemp2;
                            break;
                        }
                        case T_SOUND:
                        {
                            char *animSoundsEnd = NULL;
                            if (scriptfile_getbraces(pScript, &animSoundsEnd))
                                break;
                            parsedefinitions_game_animsounds(pScript, animSoundsEnd, fileName, animPtr);
                            break;
                        }
                        case T_FORCEFILTER:
                            animPtr->frameflags |= CUTSCENE_FORCEFILTER;
                            break;
                        case T_FORCENOFILTER:
                            animPtr->frameflags |= CUTSCENE_FORCENOFILTER;
                            break;
                        case T_TEXTUREFILTER:
                            animPtr->frameflags |= CUTSCENE_TEXTUREFILTER;
                            break;
                    }
                }
            }
            else
                pScript->textptr = animEnd+1;
        }
        break;
        case T_ANIMSOUNDS:
        {
            char *tokenPtr     = pScript->ltextptr;
            char *fileName     = NULL;

            scriptfile_getstring(pScript, &fileName);
            if (!fileName)
                break;

            char *animSoundsEnd = NULL;

            if (scriptfile_getbraces(pScript, &animSoundsEnd))
                break;

            if (firstPass)
            {
                pScript->textptr = animSoundsEnd+1;
                break;
            }

            dukeanim_t *animPtr = Anim_Find(fileName);

            if (!animPtr)
            {
                Printf("Error: expected animation filename on line %s:%d\n",
                    pScript->filename, scriptfile_getlinum(pScript, tokenPtr));
                break;
            }

            parsedefinitions_game_animsounds(pScript, animSoundsEnd, fileName, animPtr);
        }
        break;

        case T_SOUND:
        {
            char *tokenPtr = pScript->ltextptr;
            char *fileName = NULL;
            char *musicEnd;

            double volume = 1.0;

            int32_t soundNum = -1;
            int32_t maxpitch = 0;
            int32_t minpitch = 0;
            int32_t priority = 0;
            int32_t type     = 0;
            int32_t distance = 0;

            if (scriptfile_getbraces(pScript, &musicEnd))
                break;

            while (pScript->textptr < musicEnd)
            {
                switch (getatoken(pScript, soundTokens, ARRAY_SIZE(soundTokens)))
                {
                    case T_ID:       scriptfile_getsymbol(pScript, &soundNum); break;
                    case T_FILE:     scriptfile_getstring(pScript, &fileName); break;
                    case T_MINPITCH: scriptfile_getsymbol(pScript, &minpitch); break;
                    case T_MAXPITCH: scriptfile_getsymbol(pScript, &maxpitch); break;
                    case T_PRIORITY: scriptfile_getsymbol(pScript, &priority); break;
                    case T_TYPE:     scriptfile_getsymbol(pScript, &type);     break;
                    case T_DISTANCE: scriptfile_getsymbol(pScript, &distance); break;
                    case T_VOLUME:   scriptfile_getdouble(pScript, &volume);   break;
                }
            }

            if (!firstPass)
            {
                if (soundNum==-1)
                {
                    Printf("Error: missing ID for sound definition near line %s:%d\n", pScript->filename, scriptfile_getlinum(pScript,tokenPtr));
                    break;
                }

                if (fileName == NULL || fileSystem.FileExists(fileName))
                    break;

                // maybe I should have just packed this into a sound_t and passed a reference...
                if (S_DefineSound(soundNum, fileName, minpitch, maxpitch, priority, type, distance, volume) == -1)
                    Printf("Error: invalid sound ID on line %s:%d\n", pScript->filename, scriptfile_getlinum(pScript,tokenPtr));
            }
        }
        break;
        case T_GLOBALGAMEFLAGS: scriptfile_getnumber(pScript, &duke3d_globalflags); break;
        case T_NEWGAMECHOICES:
        {
            char * newGameChoicesEnd;
            if (scriptfile_getbraces(pScript,&newGameChoicesEnd))
                break;
            if (firstPass)
            {
                pScript->textptr = newGameChoicesEnd+1;
                break;
            }

            while (pScript->textptr < newGameChoicesEnd)
            {
                switch (getatoken(pScript, newGameTokens, ARRAY_SIZE(newGameTokens)))
                {
                    case T_CHOICE:
                    {
                        char * choicePtr = pScript->ltextptr;
                        char * choiceEnd;
                        int32_t choiceID;
                        if (scriptfile_getsymbol(pScript,&choiceID))
                            break;
                        if (scriptfile_getbraces(pScript,&choiceEnd))
                            break;

                        if ((unsigned)choiceID >= MAXMENUGAMEPLAYENTRIES)
                        {
                            Printf("Error: Maximum choices exceeded near line %s:%d\n",
                                pScript->filename, scriptfile_getlinum(pScript, choicePtr));
                            pScript->textptr = choiceEnd+1;
                        }

                        MenuGameplayStemEntry & stem = g_MenuGameplayEntries[choiceID];
                        stem = MenuGameplayStemEntry{};
                        MenuGameplayEntry & entry = stem.entry;

                        while (pScript->textptr < choiceEnd)
                        {
                            switch (getatoken(pScript, newGameChoiceTokens, ARRAY_SIZE(newGameChoiceTokens)))
                            {
                                case T_CHOICE:
                                {
                                    char * subChoicePtr = pScript->ltextptr;
                                    char * subChoiceEnd;
                                    int32_t subChoiceID;
                                    if (scriptfile_getsymbol(pScript,&subChoiceID))
                                        break;
                                    if (scriptfile_getbraces(pScript,&subChoiceEnd))
                                        break;

                                    if ((unsigned)subChoiceID >= MAXMENUGAMEPLAYENTRIES)
                                    {
                                        Printf("Error: Maximum subchoices exceeded near line %s:%d\n",
                                            pScript->filename, scriptfile_getlinum(pScript, subChoicePtr));
                                        pScript->textptr = subChoiceEnd+1;
                                    }

                                    MenuGameplayEntry & subentry = stem.subentries[subChoiceID];
                                    subentry = MenuGameplayEntry{};

                                    while (pScript->textptr < subChoiceEnd)
                                    {
                                        switch (getatoken(pScript, newGameSubchoiceTokens, ARRAY_SIZE(newGameSubchoiceTokens)))
                                        {
                                            case T_NAME:
                                            {
                                                char *name = NULL;
                                                if (scriptfile_getstring(pScript, &name))
                                                    break;

                                                memset(subentry.name, 0, ARRAY_SIZE(subentry.name));
                                                strncpy(subentry.name, name, ARRAY_SIZE(subentry.name)-1);
                                                break;
                                            }
                                            case T_LOCKED:
                                            {
                                                subentry.flags |= MGE_Locked;
                                                break;
                                            }
                                            case T_HIDDEN:
                                            {
                                                subentry.flags |= MGE_Hidden;
                                                break;
                                            }
                                        }
                                    }

                                    break;
                                }
                                case T_NAME:
                                {
                                    char *name = NULL;
                                    if (scriptfile_getstring(pScript, &name))
                                        break;

                                    memset(entry.name, 0, ARRAY_SIZE(entry.name));
                                    strncpy(entry.name, name, ARRAY_SIZE(entry.name)-1);
                                    break;
                                }
                                case T_LOCKED:
                                {
                                    entry.flags |= MGE_Locked;
                                    break;
                                }
                                case T_HIDDEN:
                                {
                                    entry.flags |= MGE_Hidden;
                                    break;
                                }
                                case T_USERCONTENT:
                                {
                                    entry.flags |= MGE_UserContent;
                                    break;
                                }
                            }
                        }

                        break;
                    }
                }
            }

            break;
        }
        case T_EOF: return 0;
        default: break;
        }
    }
    while (1);

    return 0;
}

int loaddefinitions_game(const char *fileName, int32_t firstPass)
{
    scriptfile *pScript = scriptfile_fromfile(fileName);

    if (pScript)
        parsedefinitions_game(pScript, firstPass);

    if (userConfig.AddDefs) for (auto& m : *userConfig.AddDefs)
        parsedefinitions_game_include(m, NULL, "null", firstPass);

    if (pScript)
        scriptfile_close(pScript);

    scriptfile_clearsymbols();

    return 0;
}



static void G_FreeHashAnim(const char * /*string*/, intptr_t key)
{
    Xfree((void *)key);
}

static void G_Cleanup(void)
{
    int32_t i;

    for (i=(MAXLEVELS*(MAXVOLUMES+1))-1; i>=0; i--) // +1 volume for "intro", "briefing" music
    {
        G_FreeMapState(i);
    }

    for (i=MAXPLAYERS-1; i>=0; i--)
    {
        Xfree(g_player[i].ps);
        Xfree(g_player[i].input);
    }

#if !defined LUNATIC
    if (label != (char *)&sprite[0]) Xfree(label);
    if (labelcode != (int32_t *)&sector[0]) Xfree(labelcode);
    if (labeltype != (uint8_t*)&wall[0]) Xfree(labeltype);
    Xfree(apScript);
    Xfree(bitptr);

//    Xfree(MusicPtr);

    Gv_Clear();

    hash_free(&h_gamevars);
    hash_free(&h_arrays);
    hash_free(&h_labels);
#endif
}

/*
===================
=
= ShutDown
=
===================
*/

void G_Shutdown(void)
{
}

/*
===================
=
= G_Startup
=
===================
*/

static void G_CompileScripts(void)
{
#if !defined LUNATIC
    label     = (char *)&sprite[0];     // V8: 16384*44/64 = 11264  V7: 4096*44/64 = 2816
    labelcode = (int32_t *)&sector[0];  // V8: 4096*40/4 = 40960    V7: 1024*40/4 = 10240
    labeltype = (uint8_t *)&wall[0];    // V8: 16384*32 = 524288    V7: 8192*32/4 = 262144
#endif

#if defined LUNATIC
    Gv_Init();
    C_InitProjectiles();
#else
    C_Compile(G_ConFile());

	if ((uint32_t)g_labelCnt > MAXSPRITES*sizeof(spritetype)/64)   // see the arithmetic above for why
        G_GameExit("Error: too many labels defined!");

    auto newlabel     = (char *)Xmalloc(g_labelCnt << 6);
    auto newlabelcode = (int32_t *)Xmalloc(g_labelCnt * sizeof(int32_t));
    auto newlabeltype = (uint8_t *)Xmalloc(g_labelCnt * sizeof(uint8_t));

    Bmemcpy(newlabel, label, g_labelCnt * 64);
    Bmemcpy(newlabelcode, labelcode, g_labelCnt * sizeof(int32_t));
    Bmemcpy(newlabeltype, labeltype, g_labelCnt * sizeof(uint8_t));

    label     = newlabel;
    labelcode = newlabelcode;
    labeltype = newlabeltype;

    Bmemset(sprite, 0, MAXSPRITES*sizeof(spritetype));
    Bmemset(sector, 0, MAXSECTORS*sizeof(sectortype));
    Bmemset(wall, 0, MAXWALLS*sizeof(walltype));

    VM_OnEvent(EVENT_INIT);
#endif
}

static inline void G_CheckGametype(void)
{
    m_coop = clamp(*m_coop, 0, g_gametypeCnt-1);
    Printf("%s\n",g_gametypeNames[m_coop]);
    if (g_gametypeFlags[m_coop] & GAMETYPE_ITEMRESPAWN)
        ud.m_respawn_items = ud.m_respawn_inventory = 1;
}

static void G_PostLoadPalette(void)
{
    if (!(duke3d_globalflags & DUKE3D_NO_HARDCODED_FOGPALS))
        lookups.setupDefaultFog();
}

#define SETFLAG(Tilenum, Flag) g_tile[Tilenum].flags |= Flag

// Has to be after setting the dynamic names (e.g. SHARK).
static void A_InitEnemyFlags(void)
{
#ifndef EDUKE32_STANDALONE
    int DukeEnemies[] = {
        SHARK, RECON, DRONE,
        LIZTROOPONTOILET, LIZTROOPJUSTSIT, LIZTROOPSTAYPUT, LIZTROOPSHOOT,
        LIZTROOPJETPACK, LIZTROOPSHOOT, LIZTROOPDUCKING, LIZTROOPRUNNING, LIZTROOP,
        OCTABRAIN, COMMANDER, COMMANDERSTAYPUT, PIGCOP, PIGCOPSTAYPUT, PIGCOPDIVE, EGG,
        LIZMAN, LIZMANSPITTING, LIZMANJUMP, ORGANTIC,
        BOSS1, BOSS2, BOSS3, BOSS4, RAT, ROTATEGUN };

    int SolidEnemies[] = { TANK, BOSS1, BOSS2, BOSS3, BOSS4, RECON, ROTATEGUN };
    int NoWaterDipEnemies[] = { OCTABRAIN, COMMANDER, DRONE };
    int GreenSlimeFoodEnemies[] = { LIZTROOP, LIZMAN, PIGCOP, NEWBEAST };

    for (bssize_t i=GREENSLIME; i<=GREENSLIME+7; i++)
        SETFLAG(i, SFLAG_HARDCODED_BADGUY);

    for (bssize_t i=ARRAY_SIZE(DukeEnemies)-1; i>=0; i--)
        SETFLAG(DukeEnemies[i], SFLAG_HARDCODED_BADGUY);

    for (bssize_t i=ARRAY_SIZE(SolidEnemies)-1; i>=0; i--)
        SETFLAG(SolidEnemies[i], SFLAG_NODAMAGEPUSH);

    for (bssize_t i=ARRAY_SIZE(NoWaterDipEnemies)-1; i>=0; i--)
        SETFLAG(NoWaterDipEnemies[i], SFLAG_NOWATERDIP);

    for (bssize_t i=ARRAY_SIZE(GreenSlimeFoodEnemies)-1; i>=0; i--)
        SETFLAG(GreenSlimeFoodEnemies[i], SFLAG_GREENSLIMEFOOD);

    if (WORLDTOUR)
    {
        SETFLAG(FIREFLY, SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS5, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS1STAYPUT, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS2STAYPUT, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS3STAYPUT, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS4STAYPUT, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
        SETFLAG(BOSS5STAYPUT, SFLAG_NODAMAGEPUSH|SFLAG_HARDCODED_BADGUY);
    }
#endif
}
#undef SETFLAG

#ifdef LUNATIC
// Will be used to store CON code translated to Lua.
int32_t g_elCONSize;
char *g_elCON;  // NOT 0-terminated!

LUNATIC_EXTERN void El_SetCON(const char *conluacode)
{
    int32_t slen = Bstrlen(conluacode);

    g_elCON = (char *)Xmalloc(slen);

    g_elCONSize = slen;
    Bmemcpy(g_elCON, conluacode, slen);
}

void El_CreateGameState(void)
{
    int32_t i;

    El_DestroyState(&g_ElState);

    if ((i = El_CreateState(&g_ElState, "game")))
    {
        Printf("Lunatic: Error initializing global ELua state (code %d)\n", i);
    }
    else
    {
        extern const char luaJIT_BC__defs_game[];

        if ((i = L_RunString(&g_ElState, luaJIT_BC__defs_game,
                             LUNATIC_DEFS_BC_SIZE, "_defs_game.lua")))
        {
            Printf("Lunatic: Error preparing global ELua state (code %d)\n", i);
            El_DestroyState(&g_ElState);
        }
    }

    if (i)
        G_GameExit("Failure setting up Lunatic!");

# if !defined DEBUGGINGAIDS
    El_ClearErrors();
# endif
}
#endif

// Throw in everything here that needs to be called after a Lua game state
// recreation (or on initial startup in a non-Lunatic build.)
void G_PostCreateGameState(void)
{
    Net_SendClientInfo();
    A_InitEnemyFlags();
}

static void G_Startup(void)
{
    int32_t i;

    timerInit(TICRATE);
    timerSetCallback(gameTimerHandler);

    G_CompileScripts();

    if (engineInit())
        G_FatalEngineError();

#ifdef LUNATIC
    El_CreateGameState();
    C_InitQuotes();
#endif

    G_InitDynamicTiles();
    G_InitDynamicSounds();

    // These depend on having the dynamic tile and/or sound mappings set up:
    G_InitMultiPsky(CLOUDYOCEAN, MOONSKY1, BIGORBIT1, LA);
    Gv_FinalizeWeaponDefaults();
    G_PostCreateGameState();
#ifdef LUNATIC
    // NOTE: This is only effective for CON-defined EVENT_INIT. See EVENT_INIT
    // not in _defs_game.lua.
    VM_OnEvent(EVENT_INIT);
#endif
    if (g_netServer || ud.multimode > 1) G_CheckGametype();

    if (userConfig.CommandMap.IsNotEmpty())
    {
		FString startupMap;
        if (VOLUMEONE)
        {
            Printf("The -map option is available in the registered version only!\n");
        }
        else
        {
			startupMap = userConfig.CommandMap;
			if (startupMap.IndexOfAny("/\\") < 0) startupMap.Insert(0, "/");
			DefaultExtension(startupMap, ".map");
			startupMap.Substitute("\\", "/");
			NormalizeFileName(startupMap);

			if (fileSystem.FileExists(startupMap))
			{
                Printf("Using level: \"%s\".\n",startupMap.GetChars());
            }
            else
            {
                Printf("Level \"%s\" not found.\n",startupMap.GetChars());
                boardfilename[0] = 0;
            }
        }
		strncpy(boardfilename, startupMap, BMAX_PATH);
    }

    for (i=0; i<MAXPLAYERS; i++)
        g_player[i].pingcnt = 0;

    Net_GetPackets();

    if (numplayers > 1)
        Printf("Multiplayer initialized.\n");

    if (TileFiles.artLoadFiles("tiles%03i.art") < 0)
        G_GameExit("Failed loading art.");

    // Make the fullscreen nuke logo background non-fullbright.  Has to be
    // after dynamic tile remapping (from C_Compile) and loading tiles.
    picanm[LOADSCREEN].sf |= PICANM_NOFULLBRIGHT_BIT;

//    Printf("Loading palette/lookups...\n");
    G_LoadLookups();

    screenpeek = myconnectindex;
}

static void P_SetupMiscInputSettings(void)
{
    auto ps = g_player[myconnectindex].ps;

    ps->aim_mode = in_mousemode;
    ps->auto_aim = cl_autoaim;
    ps->weaponswitch = cl_weaponswitch;
}

void G_UpdatePlayerFromMenu(void)
{
    if (ud.recstat != 0)
        return;

    auto &p = *g_player[myconnectindex].ps;

    if (numplayers > 1)
    {
        Net_SendClientInfo();
        if (sprite[p.i].picnum == APLAYER && sprite[p.i].pal != 1)
            sprite[p.i].pal = g_player[myconnectindex].pcolor;
    }
    else
    {
        /*int32_t j = p.team;*/

        P_SetupMiscInputSettings();
        p.palookup = g_player[myconnectindex].pcolor = G_CheckPlayerColor(playercolor);

        g_player[myconnectindex].pteam = playerteam;

        if (sprite[p.i].picnum == APLAYER && sprite[p.i].pal != 1)
            sprite[p.i].pal = g_player[myconnectindex].pcolor;
    }
}

void G_BackToMenu(void)
{
    boardfilename[0] = 0;
    if (ud.recstat == 1) G_CloseDemoWrite();
    ud.warp_on = 0;
    g_player[myconnectindex].ps->gm = 0;
	M_StartControlPanel(false);
	M_SetMenu(NAME_Mainmenu);
    inputState.keyFlushChars();
}

static int G_EndOfLevel(void)
{
    auto &p = *g_player[myconnectindex].ps;

    if ((currentLevel->flags & MI_FORCEEOG)) ud.eog = 1;    // if the finished level says to end the game, end it!
    STAT_Update(ud.eog);
    P_SetGamePalette(&p, BASEPAL, 0);
    P_UpdateScreenPal(&p);

    if (p.gm & MODE_EOL)
    {
        G_CloseDemoWrite();

        ready2send = 0;

        if (p.player_par > 0 && (p.player_par < ud.playerbest || ud.playerbest < 0) && ud.display_bonus_screen == 1)
            CONFIG_SetMapBestTime(g_loadedMapHack.md4, p.player_par);

        if ((VM_OnEventWithReturn(EVENT_ENDLEVELSCREEN, p.i, myconnectindex, 0)) == 0 && ud.display_bonus_screen == 1)
        {
            int const ssize = ud.screen_size;
            ud.screen_size = 0;
            G_UpdateScreenArea();
            ud.screen_size = ssize;
            G_BonusScreen(0);
        }

        // Clear potentially loaded per-map ART only after the bonus screens.
        artClearMapArt();

        if (ud.eog || G_HaveUserMap() || (currentLevel->flags & MI_FORCEEOG))
        {
            ud.eog = 0;
            if ((!g_netServer && ud.multimode < 2))
            {
#ifndef EDUKE32_STANDALONE
                if (!VOLUMEALL)
                    G_DoOrderScreen();
#endif
                p.gm = 0;
				return 2;
            }
            else
            {
                m_level_number = 0;
                ud.level_number = 0;
            }
        }
    }

    ud.display_bonus_screen = 1;
    ready2send = 0;

    if (numplayers > 1)
        p.gm = MODE_GAME;

    if (G_EnterLevel(p.gm))
    {
        return 2;
    }

    Net_WaitForServer();
    return 1;
}

void G_MaybeAllocPlayer(int32_t pnum)
{
    if (g_player[pnum].ps == NULL)
        g_player[pnum].ps = (DukePlayer_t *)Xcalloc(1, sizeof(DukePlayer_t));
    if (g_player[pnum].input == NULL)
        g_player[pnum].input = (input_t *)Xcalloc(1, sizeof(input_t));
}



// TODO: reorder (net)actor_t to eliminate slop and update assertion
EDUKE32_STATIC_ASSERT(sizeof(actor_t)%4 == 0);
EDUKE32_STATIC_ASSERT(sizeof(DukePlayer_t)%4 == 0);

void app_loop();

static const char* actions[] = {
    "Move_Forward",
    "Move_Backward",
    "Turn_Left",
    "Turn_Right",
    "Strafe",
    "Fire",
    "Open",
    "Run",
    "Alt_Fire",	// Duke3D", Blood
    "Jump",
    "Crouch",
    "Look_Up",
    "Look_Down",
    "Look_Left",
    "Look_Right",
    "Strafe_Left",
    "Strafe_Right",
    "Aim_Up",
    "Aim_Down",
    "Weapon_1",
    "Weapon_2",
    "Weapon_3",
    "Weapon_4",
    "Weapon_5",
    "Weapon_6",
    "Weapon_7",
    "Weapon_8",
    "Weapon_9",
    "Weapon_10",
    "Inventory",
    "Inventory_Left",
    "Inventory_Right",
    "Holo_Duke",			// Duke3D", RR
    "Jetpack",
    "NightVision",
    "MedKit",
    "TurnAround",
    "SendMessage",
    "Map",
    "Shrink_Screen",
    "Enlarge_Screen",
    "Center_View",
    "Holster_Weapon",
    "Show_Opponents_Weapon",
    "Map_Follow_Mode",
    "See_Coop_View",
    "Mouse_Aiming",
    "Toggle_Crosshair",
    "Steroids",
    "Quick_Kick",
    "Next_Weapon",
    "Previous_Weapon",
    "Show_Console",            // unused, but re-inserted for padding/compatibility for IF.
    "Show_DukeMatch_Scores",
    "Dpad_Select",
    "Dpad_Aiming",
    "Autorun",                 // unused, but re-inserted for padding/compatibility for IF.
    "Last_Weapon",
    "Quick_Save",              // unused, but re-inserted for padding/compatibility for IF.
    "Quick_Load",              // unused, but re-inserted for padding/compatibility for IF.
    "Alt_Weapon",
    "Third_Person_View",
    "Toggle_Crouch",	// This is the last one used by EDuke32.
};
int GameInterface::app_main()
{
    buttonMap.SetButtons(actions, NUM_ACTIONS);
    g_skillCnt = 4;
    ud.multimode = 1;
	ud.m_monsters_off = userConfig.nomonsters;

    // This needs to happen before G_CheckCommandLine() because G_GameExit()
    // accesses g_player[0].
    G_MaybeAllocPlayer(0);

    G_CheckCommandLine();

    CONFIG_ReadSetup();

    hud_size.Callback();
    hud_scale.Callback();
    S_InitSound();

    
    G_SetupCheats();

    if (SHAREWARE)
        g_Shareware = 1;
    else
    {
		if (fileSystem.FileExists("DUKESW.BIN")) // JBF 20030810
		{
			g_Shareware = 1;
		}
	}

    // gotta set the proper title after we compile the CONs if this is the full version

    if (g_scriptDebug)
        Printf("CON debugging activated (level %d).\n",g_scriptDebug);

#ifndef NETCODE_DISABLE
    if (g_networkMode == NET_SERVER || g_networkMode == NET_DEDICATED_SERVER)
    {
        Net_InitNetwork();
    }
#endif
    numplayers = 1;
    g_mostConcurrentPlayers = ud.multimode;  // Lunatic needs this (player[] bound)

    if (!g_fakeMultiMode)
    {
        connectpoint2[0] = -1;
    }
    else
    {
        for (int i=0; i<ud.multimode-1; i++)
            connectpoint2[i] = i+1;
        connectpoint2[ud.multimode-1] = -1;

        for (int i=1; i<ud.multimode; i++)
            g_player[i].playerquitflag = 1;
    }

    Net_GetPackets();

    // NOTE: Allocating the DukePlayer_t structs has to be before compiling scripts,
    // because in Lunatic, the {pipe,trip}bomb* members are initialized.
    for (int i=0; i<MAXPLAYERS; i++)
        G_MaybeAllocPlayer(i);

    G_Startup(); // a bunch of stuff including compiling cons

    g_player[0].playerquitflag = 1;

    auto &myplayer = *g_player[myconnectindex].ps;

    myplayer.palette = BASEPAL;

    for (int i=1, j=numplayers; j<ud.multimode; j++)
    {
        Bsprintf(g_player[j].user_name,"%s %d", GStrings("PLAYER"),j+1);
        g_player[j].ps->team = g_player[j].pteam = i;
        g_player[j].ps->weaponswitch = 3;
        g_player[j].ps->auto_aim = 0;
        i = 1-i;
    }

    Anim_Init();

    const char *defsfile = G_DefFile();
    uint32_t stime = timerGetTicks();
    if (!loaddefinitionsfile(defsfile))
    {
        uint32_t etime = timerGetTicks();
        Printf("Definitions file \"%s\" loaded in %d ms.\n", defsfile, etime-stime);
    }
    loaddefinitions_game(defsfile, FALSE);

	userConfig.AddDefs.reset();

    cacheAllSounds();

    enginePostInit();

    G_PostLoadPalette();

    tileDelete(MIRROR);

    skiptile = W_FORCEFIELD + 1;

    Gv_ResetSystemDefaults(); // called here to populate our fake tilesizx and tilesizy arrays presented to CON with sizes generated by dummytiles

    if (numplayers == 1 && boardfilename[0] != 0)
    {
        m_level_number  = 7;
        ud.m_volume_number = 0;
        ud.warp_on         = 1;
    }

    // getnames();

    if (g_netServer || ud.multimode > 1)
    {
        if (ud.warp_on == 0)
        {
            ud.m_monsters_off = 1;
            ud.m_player_skill = 0;
        }
    }

    g_mostConcurrentPlayers = ud.multimode;  // XXX: redundant?

    ud.last_level = -1;

	VM_OnEvent(EVENT_SETDEFAULTS, g_player[myconnectindex].ps->i, myconnectindex);

    registerosdcommands();

    int const clipMapError = engineLoadClipMaps();
    if (clipMapError > 0)
        Printf("There was an error loading the sprite clipping map (status %d).\n", clipMapError);

    g_clipMapFiles.Reset();

    if (g_networkMode != NET_DEDICATED_SERVER)
    {
        videoInit();
        videoSetPalette(0, myplayer.palette, 0);
    }

    // check if the minifont will support lowercase letters (3136-3161)
    // there is room for them in tiles012.art between "[\]^_." and "{|}~"
    minitext_lowercase = 1;

    for (int i = MINIFONT + ('a'-'!'); minitext_lowercase && i < MINIFONT + ('z'-'!') + 1; ++i)
        minitext_lowercase &= (int)tileCheck(i);

    if (g_networkMode != NET_DEDICATED_SERVER)
    {
        Menu_Init();
    }

    FX_StopAllSounds();
    S_ClearSoundLocks();

    //    getpackets();

    VM_OnEvent(EVENT_INITCOMPLETE);
	
	app_loop();
	return 0;
}

void app_loop()
{
	auto &myplayer = *g_player[myconnectindex].ps;

MAIN_LOOP_RESTART:
    totalclock = 0;
    ototalclock = 0;
    lockclock = 0;

    myplayer.fta = 0;
    for (int32_t & q : user_quote_time)
        q = 0;

    if(g_netClient)
    {
        Printf("Waiting for initial snapshot...");
        Net_WaitForInitialSnapshot();


    }

    if (ud.warp_on == 1)
    {
        G_NewGame_EnterLevel();
        // may change ud.warp_on in an error condition
    }

    if (ud.warp_on == 0)
    {
        if ((g_netServer || ud.multimode > 1) && boardfilename[0] != 0)
        {
            m_level_number     = 7;
            ud.m_volume_number    = 0;
            ud.m_respawn_monsters = !!(ud.m_player_skill == 4);

            for (int TRAVERSE_CONNECT(i))
            {
                P_ResetWeapons(i);
                P_ResetInventory(i);
            }

            G_NewGame_EnterLevel();

            Net_WaitForServer();
        }
        else if (g_networkMode != NET_DEDICATED_SERVER)
            G_DisplayLogo();

        if (g_networkMode != NET_DEDICATED_SERVER)
        {
			M_StartControlPanel(false);
			M_SetMenu(NAME_Mainmenu);

            if (G_PlaybackDemo())
            {
                FX_StopAllSounds();
                g_noLogoAnim = 1;
                goto MAIN_LOOP_RESTART;
            }
        }
    }
    else G_UpdateScreenArea();


//    G_GameExit(" "); ///

    ud.showweapons = ud.config.ShowWeapons;
    P_SetupMiscInputSettings();
    g_player[myconnectindex].pteam = playerteam;

    if (g_gametypeFlags[ud.coop] & GAMETYPE_TDM)
        myplayer.palookup = g_player[myconnectindex].pcolor = G_GetTeamPalette(g_player[myconnectindex].pteam);
    else
    {
        if (playercolor) myplayer.palookup = g_player[myconnectindex].pcolor = G_CheckPlayerColor(playercolor);
        else myplayer.palookup = g_player[myconnectindex].pcolor;
    }

    ud.warp_on = 0;
	inputState.ClearKeyStatus(sc_Pause);   // JBF: I hate the pause key

    if(g_netClient)
    {
        ready2send = 1; // TESTING
    }

    do //main loop
    {
		gameHandleEvents();
		if (myplayer.gm == MODE_DEMO)
		{
			M_ClearMenus();
			goto MAIN_LOOP_RESTART;
		}

        // only allow binds to function if the player is actually in a game (not in a menu, typing, et cetera) or demo
        inputState.SetBindsEnabled(!!(myplayer.gm & (MODE_GAME|MODE_DEMO)));


        if (g_networkMode != NET_DEDICATED_SERVER)
            G_HandleLocalKeys();

        OSD_DispatchQueued();

        bool gameUpdate = false;
        double gameUpdateStartTime = timerGetHiTicks();

        updatePauseStatus();

        if (paused)
        {
            ototalclock = totalclock - TICSPERFRAME;
            buttonMap.ResetButtonStates();
        }
        else
        {
            while (((g_netClient || g_netServer) || (myplayer.gm & (MODE_MENU | MODE_DEMO)) == 0) && (int)(totalclock - ototalclock) >= TICSPERFRAME)
            {
                if (g_networkMode != NET_DEDICATED_SERVER)
                {
                P_GetInput(myconnectindex);

                // this is where we fill the input_t struct that is actually processed by P_ProcessInput()
                auto const pPlayer = g_player[myconnectindex].ps;
                auto const q16ang  = fix16_to_int(pPlayer->q16ang);
                auto &     input   = inputfifo[0][myconnectindex];

                input = localInput;
                input.fvel = mulscale9(localInput.fvel, sintable[(q16ang + 2560) & 2047]) +
                             mulscale9(localInput.svel, sintable[(q16ang + 2048) & 2047]);
                input.svel = mulscale9(localInput.fvel, sintable[(q16ang + 2048) & 2047]) +
                             mulscale9(localInput.svel, sintable[(q16ang + 1536) & 2047]);

                if (!FURY)
                {
                    input.fvel += pPlayer->fric.x;
                    input.svel += pPlayer->fric.y;
                }

                localInput = {};
                }

                ototalclock += TICSPERFRAME;

                if (paused == 0 && (!System_WantGuiCapture() || ud.recstat == 2 || (g_netServer || ud.multimode > 1))
                    && (myplayer.gm & MODE_GAME))
                {
                    Net_GetPackets();
                    G_DoMoveThings();
                }
            }

            gameUpdate = true;
            g_gameUpdateTime = timerGetHiTicks() - gameUpdateStartTime;

            if (g_gameUpdateAvgTime <= 0.0)
                g_gameUpdateAvgTime = g_gameUpdateTime;

            g_gameUpdateAvgTime
            = ((GAMEUPDATEAVGTIMENUMSAMPLES - 1.f) * g_gameUpdateAvgTime + g_gameUpdateTime) / ((float)GAMEUPDATEAVGTIMENUMSAMPLES);

            G_DoCheats();
        }

        if (myplayer.gm & (MODE_EOL|MODE_RESTART))
        {
            switch (G_EndOfLevel())
            {
                case 1: continue;
                case 2: goto MAIN_LOOP_RESTART;
            }
        }

        if (g_networkMode == NET_DEDICATED_SERVER)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        else if (G_FPSLimit() || g_saveRequested)
        {
            if (!g_saveRequested)
            {
                P_GetInput(myconnectindex);
            }

            int const smoothRatio = calc_smoothratio(totalclock, ototalclock);

            G_DrawRooms(screenpeek, smoothRatio);
            if (videoGetRenderMode() >= REND_POLYMOST)
                G_DrawBackground();
            G_DisplayRest(smoothRatio);
            videoNextPage();

            if (gameUpdate)
                g_gameUpdateAndDrawTime = g_beforeSwapTime/* timerGetHiTicks()*/ - gameUpdateStartTime;
        }

        // handle CON_SAVE and CON_SAVENN
        if (g_saveRequested)
        {
            inputState.keyFlushChars();

			M_Autosave();

            g_saveRequested = false;
        }

        if (myplayer.gm & MODE_DEMO)
            goto MAIN_LOOP_RESTART;
    }
    while (1);
}

int G_DoMoveThings(void)
{
    ud.camerasprite = -1;
    lockclock += TICSPERFRAME;

    // Moved lower so it is restored correctly by demo diffs:
    //if (g_earthquakeTime > 0) g_earthquakeTime--;

    if (g_RTSPlaying > 0)
        g_RTSPlaying--;

    for (int32_t & i : user_quote_time)
    {
        if (i)
        {
            if (--i > hud_messagetime)
                i = hud_messagetime;
            if (!i) pub = NUMPAGES;
        }
    }
#ifndef NETCODE_DISABLE
    // Name display when aiming at opponents
    if (cl_idplayers && (g_netServer || ud.multimode > 1)
#ifdef SPLITSCREEN_MOD_HACKS
        && !g_fakeMultiMode
#endif
        )
    {
        hitdata_t hitData;
        auto const pPlayer = g_player[screenpeek].ps;

        for (bssize_t TRAVERSE_CONNECT(i))
            if (g_player[i].ps->holoduke_on != -1)
                sprite[g_player[i].ps->holoduke_on].cstat ^= 256;

        hitscan(&pPlayer->pos, pPlayer->cursectnum, sintable[(fix16_to_int(pPlayer->q16ang) + 512) & 2047],
                sintable[fix16_to_int(pPlayer->q16ang) & 2047], fix16_to_int(F16(100) - pPlayer->q16horiz - pPlayer->q16horizoff) << 11, &hitData,
                0xffff0030);

        for (bssize_t TRAVERSE_CONNECT(i))
            if (g_player[i].ps->holoduke_on != -1)
                sprite[g_player[i].ps->holoduke_on].cstat ^= 256;

        if ((hitData.sprite >= 0) && (g_player[myconnectindex].ps->gm & MODE_MENU) == 0 &&
                sprite[hitData.sprite].picnum == APLAYER)
        {
            int const playerNum = P_Get(hitData.sprite);

            if (playerNum != screenpeek && g_player[playerNum].ps->dead_flag == 0)
            {
                if (pPlayer->fta == 0 || pPlayer->ftq == QUOTE_RESERVED3)
                {
                    if (ldist(&sprite[pPlayer->i], &sprite[hitData.sprite]) < 9216)
                    {
						quoteMgr.InitializeQuote(QUOTE_RESERVED3, "%s", &g_player[playerNum].user_name[0]);
                        pPlayer->fta = 12, pPlayer->ftq = QUOTE_RESERVED3;
                    }
                }
                else if (pPlayer->fta > 2) pPlayer->fta -= 3;
            }
        }
    }
#endif
    if (g_showShareware > 0)
    {
        g_showShareware--;
        if (g_showShareware == 0)
        {
            pus = NUMPAGES;
            pub = NUMPAGES;
        }
    }

    // Moved lower so it is restored correctly by diffs:
//    everyothertime++;

    if (g_netClient) // [75] The server should not overwrite its own randomseed
        randomseed = ticrandomseed;

    for (bssize_t TRAVERSE_CONNECT(i))
        Bmemcpy(g_player[i].input, &inputfifo[(g_netServer && myconnectindex == i)][i], sizeof(input_t));

    G_UpdateInterpolations();

    /*
        j = -1;
        for (TRAVERSE_CONNECT(i))
        {
            if (g_player[i].playerquitflag == 0 || TEST_SYNC_KEY(g_player[i].sync->bits,SK_GAMEQUIT) == 0)
            {
                j = i;
                continue;
            }

            G_CloseDemoWrite();

            g_player[i].playerquitflag = 0;
        }
    */

    g_moveThingsCount++;

    if (ud.recstat == 1) G_DemoRecord();

    everyothertime++;
    if (g_earthquakeTime > 0) g_earthquakeTime--;

        g_globalRandom = krand();
        A_MoveDummyPlayers();//ST 13

    for (bssize_t TRAVERSE_CONNECT(i))
    {
        if (g_player[i].ps->team != g_player[i].pteam && g_gametypeFlags[ud.coop] & GAMETYPE_TDM)
        {
            g_player[i].ps->team = g_player[i].pteam;
            actor[g_player[i].ps->i].picnum = APLAYERTOP;
            P_QuickKill(g_player[i].ps);
        }

        if (g_gametypeFlags[ud.coop] & GAMETYPE_TDM)
            g_player[i].ps->palookup = g_player[i].pcolor = G_GetTeamPalette(g_player[i].ps->team);

        if (sprite[g_player[i].ps->i].pal != 1)
            sprite[g_player[i].ps->i].pal = g_player[i].pcolor;

        P_HandleSharedKeys(i);

            P_ProcessInput(i);
            P_CheckSectors(i);
        }

        G_MoveWorld();

//    Net_CorrectPrediction();

    if (g_netServer)
        Net_SendServerUpdates();

    if ((everyothertime&1) == 0)
    {
        G_AnimateWalls();
        A_MoveCyclers();

        if ((everyothertime % 10) == 0)
        {
            if(g_netServer)
            {
                Net_SendMapUpdate();
            }
            else if(g_netClient)
            {
                Net_StoreClientState();
            }
        }
    }

    if (g_netClient)   //Slave
        Net_SendClientUpdate();

    return 0;
}

#ifndef EDUKE32_STANDALONE
void A_SpawnWallGlass(int spriteNum, int wallNum, int glassCnt)
{
    if (wallNum < 0)
    {
        for (bssize_t j = glassCnt - 1; j >= 0; --j)
        {
            int const a = SA(spriteNum) - 256 + (krand() & 511) + 1024;
            A_InsertSprite(SECT(spriteNum), SX(spriteNum), SY(spriteNum), SZ(spriteNum), GLASSPIECES + (j % 3), -32, 36, 36, a,
                           32 + (krand() & 63), 1024 - (krand() & 1023), spriteNum, 5);
        }
        return;
    }

    vec2_t v1 = { wall[wallNum].x, wall[wallNum].y };
    vec2_t v  = { wall[wall[wallNum].point2].x - v1.x, wall[wall[wallNum].point2].y - v1.y };

    v1.x -= ksgn(v.y);
    v1.y += ksgn(v.x);

    v.x = tabledivide32_noinline(v.x, glassCnt+1);
    v.y = tabledivide32_noinline(v.y, glassCnt+1);

    int16_t sect = -1;

    for (int j = glassCnt; j > 0; --j)
    {
        v1.x += v.x;
        v1.y += v.y;

        updatesector(v1.x,v1.y,&sect);
        if (sect >= 0)
        {
            int z = sector[sect].floorz - (krand() & (klabs(sector[sect].ceilingz - sector[sect].floorz)));

            if (z < -ZOFFSET5 || z > ZOFFSET5)
                z = SZ(spriteNum) - ZOFFSET5 + (krand() & ((64 << 8) - 1));

            A_InsertSprite(SECT(spriteNum), v1.x, v1.y, z, GLASSPIECES + (j % 3), -32, 36, 36, SA(spriteNum) - 1024, 32 + (krand() & 63),
                           -(krand() & 1023), spriteNum, 5);
        }
    }
}

void A_SpawnGlass(int spriteNum, int glassCnt)
{
    for (; glassCnt>0; glassCnt--)
    {
        int const k
        = A_InsertSprite(SECT(spriteNum), SX(spriteNum), SY(spriteNum), SZ(spriteNum) - ((krand() & 16) << 8), GLASSPIECES + (glassCnt % 3),
                         krand() & 15, 36, 36, krand() & 2047, 32 + (krand() & 63), -512 - (krand() & 2047), spriteNum, 5);
        sprite[k].pal = sprite[spriteNum].pal;
    }
}

void A_SpawnCeilingGlass(int spriteNum, int sectNum, int glassCnt)
{
    int const startWall = sector[sectNum].wallptr;
    int const endWall = startWall+sector[sectNum].wallnum;

    for (bssize_t wallNum = startWall; wallNum < (endWall - 1); wallNum++)
    {
        vec2_t v1 = { wall[wallNum].x, wall[wallNum].y };
        vec2_t v  = { tabledivide32_noinline(wall[wallNum + 1].x - v1.x, glassCnt + 1),
                     tabledivide32_noinline(wall[wallNum + 1].y - v1.y, glassCnt + 1) };

        for (int j = glassCnt; j > 0; j--)
        {
            v1.x += v.x;
            v1.y += v.y;
            A_InsertSprite(sectNum, v1.x, v1.y, sector[sectNum].ceilingz + ((krand() & 15) << 8), GLASSPIECES + (j % 3), -32, 36, 36,
                           krand() & 2047, (krand() & 31), 0, spriteNum, 5);
        }
    }
}

void A_SpawnRandomGlass(int spriteNum, int wallNum, int glassCnt)
{
    if (wallNum < 0)
    {
        for (bssize_t j = glassCnt - 1; j >= 0; j--)
        {
            int const k
            = A_InsertSprite(SECT(spriteNum), SX(spriteNum), SY(spriteNum), SZ(spriteNum) - (krand() & (63 << 8)), GLASSPIECES + (j % 3),
                             -32, 36, 36, krand() & 2047, 32 + (krand() & 63), 1024 - (krand() & 2047), spriteNum, 5);
            sprite[k].pal = krand() & 15;
        }
        return;
    }

    vec2_t v1 = { wall[wallNum].x, wall[wallNum].y };
    vec2_t v  = { tabledivide32_noinline(wall[wall[wallNum].point2].x - wall[wallNum].x, glassCnt + 1),
                 tabledivide32_noinline(wall[wall[wallNum].point2].y - wall[wallNum].y, glassCnt + 1) };
    int16_t sectNum = sprite[spriteNum].sectnum;

    for (int j = glassCnt; j > 0; j--)
    {
        v1.x += v.x;
        v1.y += v.y;

        updatesector(v1.x, v1.y, &sectNum);

        int z = sector[sectNum].floorz - (krand() & (klabs(sector[sectNum].ceilingz - sector[sectNum].floorz)));

        if (z < -ZOFFSET5 || z > ZOFFSET5)
            z       = SZ(spriteNum) - ZOFFSET5 + (krand() & ((64 << 8) - 1));

        int const k = A_InsertSprite(SECT(spriteNum), v1.x, v1.y, z, GLASSPIECES + (j % 3), -32, 36, 36, SA(spriteNum) - 1024,
                                     32 + (krand() & 63), -(krand() & 2047), spriteNum, 5);
        sprite[k].pal = krand() & 7;
    }
}
#endif

#if 0
void GameInterface::SendMessage(const char* msg)
{
    if ((g_netServer || ud.multimode > 1) && buttonMap.ButtonDown(gamefunc_SendMessage))
    {
        inputState.keyFlushChars();
        buttonMap.ClearButton(gamefunc_SendMessage);
        myplayer.gm |= MODE_TYPE;
        typebuf[0] = 0;
    }
}
 
void nix()
    {
    int32_t const hitstate = I_EnterText(typebuf, 120, 0);

    int32_t const y = ud.screen_size > 1 ? (200 - 58) << 16 : (200 - 35) << 16;

    int32_t const width = mpgametextsize(typebuf, TEXT_LITERALESCAPE).x;
    int32_t const fullwidth = width + textsc((tilesiz[SPINNINGNUKEICON].x << 15) + (2 << 16));
    int32_t const text_x = fullwidth >= (320 << 16) ? (320 << 16) - fullwidth : mpgametext_x;
    mpgametext(text_x, y, typebuf, 1, 2 | 8 | 16 | ROTATESPRITE_FULL16, 0, TEXT_YCENTER | TEXT_LITERALESCAPE);
    int32_t const cursor_x = text_x + width + textsc((tilesiz[SPINNINGNUKEICON].x << 14) + (1 << 16));
    rotatesprite_fs(cursor_x, y, textsc(32768), 0, SPINNINGNUKEICON + (((int32_t)totalclock >> 3) % 7),
        4 - (sintable[((int32_t)totalclock << 4) & 2047] >> 11), 0, 2 | 8);

    if (hitstate == 1)
    {
        inputState.ClearKeyStatus(sc_Enter);
        if (Bstrlen(typebuf) == 0)
        {
            g_player[myconnectindex].ps->gm &= ~(MODE_TYPE | MODE_SENDTOWHOM);
            return;
        }
        if (cl_automsg)
        {
            if (SHIFTS_IS_PRESSED)
                g_chatPlayer = -1;
            else
                g_chatPlayer = ud.multimode;
        }
        g_player[myconnectindex].ps->gm |= MODE_SENDTOWHOM;
    }
    else if (hitstate == -1)
        g_player[myconnectindex].ps->gm &= ~(MODE_TYPE | MODE_SENDTOWHOM);
    else
        pub = NUMPAGES;
    }
}
#endif

void GameInterface::FreeGameData()
{
    G_Cleanup();
}

void GameInterface::UpdateScreenSize()
{
    G_UpdateScreenArea();
}

::GameInterface* CreateInterface()
{
	return new GameInterface;
}

END_DUKE_NS
