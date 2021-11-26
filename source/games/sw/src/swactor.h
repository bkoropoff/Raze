#pragma once

// included by game.h

BEGIN_SW_NS


class DSWActor : public DCoreActor
{
	DSWActor* base();

public:

	bool hasUser;
	USER user;
	walltype* tempwall;	// transient, to replace a hack using a 16 bit sprite field.

	DSWActor()
	{
		index = (int(this - base()));
	}
	DSWActor& operator=(const DSWActor& other) = default;

	void Clear()
	{
		clearUser();
	}
	bool hasU() { return hasUser; }


	USER* u() { return &user; }
	USER* allocUser() 
	{ 
		hasUser = true;
		return u(); 
	}

	void clearUser()
	{
		hasUser = false;
		user.Clear();
	}
};

extern DSWActor swActors[MAXSPRITES];

inline DSWActor* DSWActor::base() { return swActors; }

// subclassed to add a game specific actor() method

// Iterator wrappers that return an actor pointer, not an index.
using SWStatIterator = TStatIterator<DSWActor>;
using SWSectIterator = TSectIterator<DSWActor>;
using SWSpriteIterator = TSpriteIterator<DSWActor>;
using SWLinearSpriteIterator = TLinearSpriteIterator<DSWActor>;


inline FSerializer& Serialize(FSerializer& arc, const char* keyname, DSWActor*& w, DSWActor** def)
{
	int index = w? int(w - swActors) : -1;
	Serialize(arc, keyname, index, nullptr);
	if (arc.isReading()) w = index == -1? nullptr : &swActors[index];
	return arc;
}

inline void ChangeActorSect(DSWActor* actor, int sect)
{
	changespritesect(actor->GetSpriteIndex(), sect);
}

inline void ChangeActorSect(DSWActor* actor, sectortype* sect)
{
	changespritesect(actor->GetSpriteIndex(), sectnum(sect));
}

inline int SetActorZ(DSWActor* actor, const vec3_t* newpos)
{
	return setspritez(actor->GetSpriteIndex(), newpos);
}

inline int SetActor(DSWActor* actor, const vec3_t* newpos)
{
	return setsprite(actor->GetSpriteIndex(), newpos);
}

END_SW_NS
