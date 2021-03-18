// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_flat.cpp
** Flat processing
**
*/

#include "matrix.h"
#include "hw_dynlightdata.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_material.h"
#include "hw_drawinfo.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_drawstructs.h"
#include "hw_renderstate.h"
#include "texturemanager.h"
#include "earcut.hpp"

#ifdef _DEBUG
CVAR(Int, gl_breaksec, -1, 0)
#endif

extern PalEntry GlobalMapFog;
extern float GlobalFogDensity;

//==========================================================================
//
//
//
//==========================================================================
#if 0
void HWFlat::SetupLights(HWDrawInfo *di, FLightNode * node, FDynLightData &lightdata, int portalgroup)
{
	Plane p;

	lightdata.Clear();
	if (renderstyle == STYLE_Add && !di->Level->lightadditivesurfaces)
	{
		dynlightindex = -1;
		return;	// no lights on additively blended surfaces.
	}
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		if (!light->IsActive())
		{
			node = node->nextLight;
			continue;
		}
		iter_dlightf++;

		// we must do the side check here because gl_GetLight needs the correct plane orientation
		// which we don't have for Legacy-style 3D-floors
		double planeh = plane.plane.ZatPoint(light->Pos);
		if ((planeh<light->Z() && ceiling) || (planeh>light->Z() && !ceiling))
		{
			node = node->nextLight;
			continue;
		}

		p.Set(plane.plane.Normal(), plane.plane.fD());
		draw_dlightf += GetLight(lightdata, portalgroup, p, light, false);
		node = node->nextLight;
	}

	dynlightindex = screen->mLights->UploadLights(lightdata);
}
#endif

//==========================================================================
//
// CalcPlane fixme - this should be stored in the sector, not be recalculated each frame.
//
//==========================================================================

static FVector3 CalcNormal(sectortype* sector, int plane)
{
	FVector3 pt[3];

	auto wal = &wall[sector->wallptr];
	auto wal2 = &wall[wal->point2];

	pt[0] = { (float)WallStartX(wal), (float)WallStartY(wal), 0 };
	pt[1] = { (float)WallEndX(wal), (float)WallEndY(wal), 0 };
	PlanesAtPoint(sector, wal->x, wal->y, plane ? &pt[0].Z : nullptr, plane? nullptr : &pt[0].Z);
	PlanesAtPoint(sector, wal2->x, wal2->y, plane ? &pt[1].Z : nullptr, plane ? nullptr : &pt[1].Z);

	if (pt[0].X == pt[1].X)
	{
		if (pt[0].Y == pt[1].Y) return { 0.f, 0.f, plane ? -1.f : 1.f };
		pt[2].X = pt[0].X + 4;
		pt[2].Y = pt[0].Y;
	}
	else
	{
		pt[2].X = pt[0].X;
		pt[2].Y = pt[0].Y + 4;
	}
	PlanesAtPoint(sector, pt[2].X * 16, pt[2].Y * 16, plane ? &pt[2].Z : nullptr, plane ? nullptr : &pt[2].Z);

	auto normal = (pt[2] - pt[0]) ^ (pt[1] - pt[0]);

	if ((pt[2].Z < 0 && !plane) || (pt[2].Z > 0 && plane)) return -pt[2];
	return pt[2];
}


//==========================================================================
//
// this should be buffered later.
//
//==========================================================================

void HWFlat::MakeVertices()
{
	int numvertices = sec->wallnum;
	
	TArray<FVector3> points(numvertices, true);
	using Point = std::pair<float, float>;
	std::vector<std::vector<Point>> polygon;
	std::vector<Point>* curPoly;

	polygon.resize(1);
	curPoly = &polygon.back();

	for (int i = 0; i < numvertices; i++)
	{
		auto wal = &wall[sec->wallptr + i];

		float X = WallStartX(wal);
		float Y = WallStartY(wal);
		curPoly->push_back(std::make_pair(X, Y));
		if (wal->point2 != sec->wallptr+i+1 && i < numvertices - 1)
		{
			polygon.resize(polygon.size() + 1);
			curPoly = &polygon.back();
		}
	}
	// Now make sure that the outer boundary is the first polygon by picking a point that's as much to the outside as possible.
	int outer = 0;
	float minx = FLT_MAX;
	float miny = FLT_MAX;
	for (size_t a = 0; a < polygon.size(); a++)
	{
		for (auto& pt : polygon[a])
		{
			if (pt.first < minx || (pt.first == minx && pt.second < miny))
			{
				minx = pt.first;
				miny = pt.second;
				outer = a;
			}
		}
	}
	if (outer != 0) std::swap(polygon[0], polygon[outer]);
	auto indices = mapbox::earcut(polygon);

	int p = 0;
	for (size_t a = 0; a < polygon.size(); a++)
	{
		for (auto& pt : polygon[a])
		{
			float planez;
			PlanesAtPoint(sec, (pt.first * 16), (pt.second * -16), plane ? &planez : nullptr, !plane ? &planez : nullptr);
			FVector3 point = { pt.first, pt.second, planez };
			points[p++] = point;
		}
	}

	auto ret = screen->mVertexData->AllocVertices(indices.size());
	auto vp = ret.first;
	for (auto i : indices)
	{
		auto& pt = points[indices[i]];
		vp->SetVertex(pt.X, pt.Z, pt.Y);
		vp->SetTexCoord(pt.X / 64.f, pt.Y / 64.f);	// todo: align
		vp++;
	}
	vertindex = ret.second;
	vertcount = indices.size();
}

//==========================================================================
//
//
//
//==========================================================================
void HWFlat::DrawFlat(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	if (screen->BuffersArePersistent())
	{
		MakeVertices();
	}

#ifdef _DEBUG
	if (sec - sector == gl_breaksec)
	{
		int a = 0;
	}
#endif

	state.SetNormal(CalcNormal(sector, plane));

	// Fog must be done before the texture so that the texture selector can override it.
	bool foggy = (GlobalMapFog || (fade & 0xffffff));
	auto ShadeDiv = lookups.tables[palette].ShadeFactor;
	// Disable brightmaps if non-black fog is used.
	if (ShadeDiv >= 1 / 1000.f && foggy)
	{
		state.EnableFog(1);
		float density = GlobalMapFog ? GlobalFogDensity : 350.f - Scale(numshades - shade, 150, numshades);
		state.SetFog((GlobalMapFog) ? GlobalMapFog : fade, density);
		state.SetSoftLightLevel(255);
		state.SetLightParms(128.f, 1 / 1000.f);
	}
	else
	{
		state.EnableFog(0);
		state.SetFog(0, 0);
		state.SetSoftLightLevel(ShadeDiv >= 1 / 1000.f ? 255 - Scale(shade, 255, numshades) : 255);
		state.SetLightParms(visibility, ShadeDiv / (numshades - 2));
	}

	// The shade rgb from the tint is ignored here.
	state.SetColor(PalEntry(255, globalr, globalg, globalb));

	if (translucent)
	{
		state.SetRenderStyle(renderstyle);
		if (!texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
		else state.AlphaFunc(Alpha_GEqual, 0.f);
	}
	state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, 0, -1);

	state.SetLightIndex(dynlightindex);
	state.Draw(DT_Triangles, vertindex, vertcount);
	vertexcount += vertcount;

	state.EnableTextureMatrix(false);

	state.SetRenderStyle(DefaultRenderStyle());
	//state.SetObjectColor(0xffffffff);
	//state.SetAddColor(0);
	//state.ApplyTextureManipulation(nullptr);
}

//==========================================================================
//
// HWFlat::PutFlat
//
// submit to the renderer
//
//==========================================================================

void HWFlat::PutFlat(HWDrawInfo *di, int whichplane)
{
	if (!screen->BuffersArePersistent())	// should be made static buffer content later (when the logic is working)
	{
#if 0
		if (di->Level->HasDynamicLights && texture != nullptr && !di->isFullbrightScene() && !(hacktype & (SSRF_PLANEHACK | SSRF_FLOODHACK)))
		{
			SetupLights(di, section->lighthead, lightdata, sector->PortalGroup);
		}
#endif
		MakeVertices();
	}
	plane = whichplane;
	di->AddFlat(this);
	rendered_flats++;
}

//==========================================================================
//
// Process a sector's flats for rendering
// This function is only called once per sector.
// Subsequent subsectors are just quickly added to the ss_renderflags array
//
//==========================================================================

void HWFlat::ProcessSector(HWDrawInfo *di, sectortype * frontsector, int which)
{
#ifdef _DEBUG
	if (frontsector - sector == gl_breaksec)
	{
		int a = 0;
	}
#endif

	dynlightindex = -1;

    const auto &vp = di->Viewpoint;

	float florz, ceilz;
	PlanesAtPoint(frontsector, vp.Pos.X, vp.Pos.Y, &ceilz, &florz);

	fade = lookups.getFade(frontsector->floorpal);	// fog is per sector.
	visibility = sectorVisibility(frontsector);
	sec = frontsector;

	//
	//
	//
	// do floors
	//
	//
	//
	if ((which & SSRF_RENDERFLOOR) && !(frontsector->floorstat & CSTAT_SECTOR_SKY) && florz <= vp.Pos.Z)
	{
		// process the original floor first.

		shade = frontsector->floorshade;
		palette = frontsector->floorpal;

		//port = frontsector->ValidatePortal(sector_t::floor);
#if 0
		if ((stack = (port != NULL)))
		{
			alpha = frontsector->GetAlpha(sector_t::floor);
		}
		else
#endif
			alpha = 1.0f;

		if (alpha != 0.f)
		{
			int tilenum = frontsector->floorpicnum;
			tileUpdatePicnum(&tilenum, tilenum, 0);
			texture = tileGetTexture(tilenum);
			if (texture && texture->isValid())
			{
				//iboindex = frontsector->iboindex[sector_t::floor];
				renderstyle = STYLE_Translucent;
				PutFlat(di, 0);
			}
		}
	}

	//
	//
	//
	// do ceilings
	//
	// 
	//
	if ((which & SSRF_RENDERCEILING) && !(frontsector->ceilingstat & CSTAT_SECTOR_SKY) && ceilz >= vp.Pos.Z)
	{
		// process the original ceiling first.

		shade = frontsector->ceilingshade;
		palette = frontsector->ceilingpal;


		/*
		port = frontsector->ValidatePortal(sector_t::ceiling);
		if ((stack = (port != NULL)))
		{
			alpha = frontsector->GetAlpha(sector_t::ceiling);
		}
		else*/
			alpha = 1.0f;

		if (alpha != 0.f)
		{
			//iboindex = frontsector->iboindex[sector_t::ceiling];

			int tilenum = frontsector->ceilingpicnum;
			tileUpdatePicnum(&tilenum, tilenum, 0);
			texture = tileGetTexture(tilenum);
			if (texture && texture->isValid())
			{
				//iboindex = frontsector->iboindex[sector_t::floor];
				renderstyle = STYLE_Translucent;
				PutFlat(di,  1);
			}
		}
	}
}

