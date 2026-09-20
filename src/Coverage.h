#pragma once

#include <API/ARK/Ark.h>
#include <string>

namespace sb
{
	// How many turrets can engage each point on a grid.
	//
	// Distance is 3D on purpose. Turret reach is 6000 units for Heavy and Tek,
	// and a tower with a spire stands about 10000 tall - so height alone can put
	// a turret out of range of the ground beneath it. Treating coverage as flat
	// circles would claim protection that does not exist.
	struct CoverageMap
	{
		static constexpr int kGrid = 21;
		int count[kGrid][kGrid]{};
		bool hit[kGrid][kGrid]{};

		int turrets_considered = 0;
		int max_count = 0;
		int covered_cells = 0;
		int total_cells = 0;
		float span = 0.f;
		float sample_z = 0.f;   // 0 = followed the ground
	};

	// `span` is the width of the sampled square in world units, centred on
	// `center`. `fixed_z` of 0 follows the terrain; otherwise every sample sits
	// at that absolute height.
	bool ScanCoverage(const FVector& center, float span, float fixed_z, CoverageMap& out);
} // namespace sb
