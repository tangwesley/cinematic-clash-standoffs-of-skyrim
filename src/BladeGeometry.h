#pragma once

// Where an actor's weapon actually is, measured off the loaded 3D.
//
// The engine hands out no blade tip. The weapon nif hangs off the skeleton's
// WEAPON node (SHIELD for the left hand) with its origin at the grip, and the
// cheapest geometry available is one bounding sphere per shape (BSGeometry's
// modelBound). A blade is the segment from the node's origin to the farthest
// point of those spheres: the tip to within the blade's own width. Vertex data
// would be exact but is not reachable through CommonLib.
namespace BladeGeometry
{
	struct Blade
	{
		bool         valid{ false };
		RE::NiPoint3 grip;               // world; the attachment node's origin, i.e. the hand
		RE::NiPoint3 tip;                // world
		bool         leftHand{ false };  // measured off SHIELD rather than WEAPON

		[[nodiscard]] float Length() const { return (tip - grip).Length(); }

		// The same blade with its owner moved. Translation only: headings are
		// locked for the whole standoff, so the orientation still holds.
		[[nodiscard]] Blade Translated(const RE::NiPoint3& a_offset) const
		{
			Blade moved = *this;
			moved.grip += a_offset;
			moved.tip += a_offset;
			return moved;
		}
	};

	// The blade this actor would meet an opponent at a_opponent with: the
	// right hand's, or the left's when that reaches nearer (dual wield, or a
	// shield left visible). Third-person 3D only; the first-person model lives
	// in the camera's own space and meets nothing.
	[[nodiscard]] Blade Measure(RE::Actor* a_actor, const RE::NiPoint3& a_opponent);

	// Shortest distance between two blades, with the closest point on each.
	// Zero means they cross.
	float Gap(const Blade& a_lhs, const Blade& a_rhs, RE::NiPoint3& a_onLhs, RE::NiPoint3& a_onRhs);

	// An actor's torso as one capsule, hips to head.
	struct Body
	{
		bool         valid{ false };
		RE::NiPoint3 low;   // hips
		RE::NiPoint3 high;  // head
		float        radius{ 0.0f };
	};

	[[nodiscard]] Body MeasureBody(RE::Actor* a_actor, float a_radius);

	// How far the blade reaches inside that body, with the deepest point on it
	// and the shortest way out. Zero or less means clear.
	float Penetration(const Blade& a_blade, const Body& a_body, RE::NiPoint3& a_onBlade, RE::NiPoint3& a_push);
}
