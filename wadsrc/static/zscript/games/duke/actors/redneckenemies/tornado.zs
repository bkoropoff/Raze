class RedneckTornado : DukeActor
{
	default
	{
		pic "TORNADO";
		+DESTRUCTOIMMUNE;
		+INTERNAL_BADGUY;
		+NOHITSCANHIT;
		Strength MEGASTRENGTH;
	}
	override void Initialize()
	{
		self.scale = (1, 2);
		self.setClipDistFromTile();
		self.clipdist *= 0.25;
		self.cstat = CSTAT_SPRITE_TRANSLUCENT;
	}
}