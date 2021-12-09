#pragma once

#include <stdint.h>
#include "build.h"

class DCoreActor : public DObject
{
	DECLARE_CLASS(DCoreActor, DObject)
	HAS_OBJECT_POINTERS
	// common part of the game actors
public:

	// These two are needed because we cannot rely on the ones in the sprites for unlinking.
	int link_stat;
	sectortype* link_sector;
	DCoreActor* prevStat, * nextStat;
	DCoreActor* prevSect, * nextSect;

	spritetype spr;
	spriteext_t sprext;
	spritesmooth_t spsmooth;

	DCoreActor() = default;
	virtual ~DCoreActor() = default;
	DCoreActor(const DCoreActor& other) = delete;				// we also do not want to allow copies.
	DCoreActor& operator=(const DCoreActor& other) = delete;

	virtual void Serialize(FSerializer& arc);
	// This may only be called when all actor lists are clean.
	virtual void ClearContent()
	{
		link_stat = MAXSTATUS;
		link_sector = nullptr;
		prevStat = nextStat = prevSect = nextSect = nullptr;
		spr = {};
	}

	virtual void BeginPlay() {}
	void OnDestroy() override;
	size_t PropagateMark() override;

	bool exists() const
	{
		return (unsigned)s().statnum < MAXSTATUS;
	}

	const spritetype& s() const
	{ 
		return spr;
	}

	spritetype& s()
	{
		return spr;
	}

	const spriteext_t& sx() const
	{
		return sprext;
	}

	spriteext_t& sx()
	{
		return sprext;
	}

	const spritesmooth_t& sm() const
	{
		return spsmooth;
	}

	spritesmooth_t& sm()
	{
		return spsmooth;
	}

	int GetIndex() const
	{ 
		// For error printing only! This is only identical with the sprite index for items spawned at map start.
		return s().time; 
	}	

	sectortype* sector() const
	{
		return s().sector();
	}

	bool insector() const
	{
		return s().insector();
	}

};

// holds pointers to the game-side actors.
extern TArray<sectortype> sector;
extern TArray<walltype> wall;


// Masking these into the object index to keep it in 16 bit was probably the single most dumbest and pointless thing Build ever did.
// Gonna be fun to globally replace these to finally lift the limit this imposes on map size.
// Names taken from DukeGDX
enum EHitBits
{
    kHitNone = 0,
    kHitTypeMask = 0xC000,
    kHitTypeMaskSW = 0x1C000, // SW has one more relevant bit
    kHitIndexMask = 0x3FFF,
    kHitSector = 0x4000,
    kHitWall = 0x8000,
    kHitSprite = 0xC000,
    kHitVoid = 0x10000,      // SW only


};

// This serves as input/output for all functions dealing with collisions, hits, etc.
// Not all utilities use all variables.
struct HitInfoBase
{
	vec3_t hitpos;
	sectortype* hitSector;
	walltype* hitWall;
    DCoreActor* hitActor;

	void clearObj()
	{
		hitSector = nullptr;
		hitWall = nullptr;
		hitActor = nullptr;
	}
};

template<class T>
struct THitInfo : public HitInfoBase
{
	T* actor() const { return static_cast<T*>(hitActor); }
};


struct CollisionBase
{
	int type;
	int exbits;	// extended game-side info (only used by Exhumed)
	union
	{
		// can only have one at a time
		sectortype* hitSector;
		walltype* hitWall;
		DCoreActor* hitActor;
	};

	void invalidate()
	{
		type = -1; // something invalid that's not a valid hit type.
		hitSector = nullptr;
	}

	int setNone()
	{
		*this = {};
		return kHitNone;
	}

	int setSector(int num)
	{
		*this = {};
		type = kHitSector;
		hitSector = &sector[num];
		return kHitSector;
	}

	int setSector(sectortype* num)
	{
		*this = {};
		type = kHitSector;
		hitSector = num;
		return kHitSector;
	}

	int setWall(int num)
	{
		*this = {};
		type = kHitWall;
		hitWall = &wall[num];
		return kHitWall;
	}

	int setWall(walltype* num)
	{
		*this = {};
		type = kHitWall;
		hitWall = num;
		return kHitWall;
	}

	int setSprite(DCoreActor* num)
	{
		*this = {};
		type = kHitSprite;
		hitActor = num;
		return kHitSprite;
	}

	int setVoid()
	{
		hitSector = nullptr;
		type = kHitVoid;
		return kHitVoid;
	}
};

template<class T>
struct TCollision : public CollisionBase
{
	T* actor() const
	{ 
		return static_cast<T*>(hitActor); 
	}

	// normally collision data is short lived, this is only needed in some very rare circumstances.
	T* safeActor()
	{
		return static_cast<T*>(GC::ReadBarrier(hitActor));
	}

	auto operator=(const CollisionBase& other)
	{
		*(CollisionBase*)this = other;
		return *this;
	}
};


struct ActorStatList
{
	DCoreActor* firstEntry, * lastEntry;
};

extern ActorStatList statList[MAXSTATUS];

// Iterator wrappers that return an actor pointer, not an index.
template<class TActor>
class TStatIterator
{
	DCoreActor* next;
public:
	TStatIterator(int stat)
	{
		next = statList[stat].firstEntry;
	}

	void Reset(int stat)
	{
		next = statList[stat].firstEntry;
	}

	TActor* Next()
	{
		auto n = next;
		if (next) next = next->nextStat;
		return static_cast<TActor*>(n);
	}

	TActor* Peek()
	{
		return static_cast<TActor*>(next);
	}
};

template<class TActor>
class TSectIterator
{
	DCoreActor* next;
public:
	//[[deprecated]]
	TSectIterator(int stat)
	{
		next = sector[stat].firstEntry;
	}

	TSectIterator(sectortype* stat)
	{
		next = stat->firstEntry;
	}

	//[[deprecated]]
	void Reset(int stat)
	{
		next = sector[stat].firstEntry;
	}

	void Reset(sectortype* stat)
	{
		next = stat->firstEntry;
	}


	TActor* Next()
	{
		auto n = next;
		if (next) next = next->nextSect;
		return static_cast<TActor*>(n);
	}

	TActor* Peek()
	{
		return static_cast<TActor*>(next);
	}
};

// An iterator to iterate over all sprites.
template<class TActor>
class TSpriteIterator
{
	TStatIterator<TActor> it;
	int stat = 0;

public:
	TSpriteIterator() : it(0) {}

	TActor* Next()
	{
		while (stat < MAXSTATUS)
		{
			auto ac = it.Next();
			if (ac) return ac;
			stat++;
			if (stat < MAXSTATUS) it.Reset(stat);
		}
		return nullptr;
	}

	void Reset()
	{
		stat = 0;
		it.Reset(0);
	}
};

using CoreSectIterator = TSectIterator<DCoreActor>;

DCoreActor* InsertActor(PClass* type, sectortype* sector, int stat, bool forcetail = false);
void ChangeActorSect(DCoreActor* actor, sectortype* sector, bool forcetail = false);
int ChangeActorStat(DCoreActor* actor, int nStatus, bool forcetail = false);
void InitSpriteLists();


void SetActorZ(DCoreActor* actor, const vec3_t* newpos);
void SetActor(DCoreActor* actor, const vec3_t* newpos);

inline void SetActor(DCoreActor* actor, const vec3_t& newpos)
{
	SetActor(actor, &newpos);
}



inline int clipmove(vec3_t& pos, sectortype** const sect, int xvect, int yvect,
	int const walldist, int const ceildist, int const flordist, unsigned const cliptype, CollisionBase& result, int clipmoveboxtracenum = 3)
{
	int sectno = *sect ? sector.IndexOf(*sect) : -1;
	result = clipmove_(&pos, &sectno, xvect, yvect, walldist, ceildist, flordist, cliptype, clipmoveboxtracenum);
	*sect = sectno == -1 ? nullptr : &sector[sectno];
	return result.type;
}

inline int pushmove(vec3_t* const vect, sectortype** const sect, int32_t const walldist, int32_t const ceildist, int32_t const flordist,
	uint32_t const cliptype, bool clear = true)
{
	int sectno = *sect ? sector.IndexOf(*sect) : -1;
	int res = pushmove_(vect, &sectno, walldist, ceildist, flordist, cliptype, clear);
	*sect = sectno == -1 ? nullptr : &sector[sectno];
	return res;
}

inline tspriteptr_t renderAddTsprite(tspritetype* tsprite, int& spritesortcnt, DCoreActor* actor)
{
	if (spritesortcnt >= MAXSPRITESONSCREEN) return nullptr;
	auto tspr = &tsprite[spritesortcnt++];
	tspr->copyfrom(&actor->s());
	tspr->clipdist = 0;
	tspr->ownerActor = actor;
	return tspr;
}
