#include "BladeGeometry.h"

namespace
{
	// Below this is an empty hand or a torch stub; above it is a magic effect
	// or a trail mesh hanging off the hand.
	constexpr float kMinBladeLength = 15.0f;
	constexpr float kMaxBladeLength = 260.0f;

	// Farthest point of any bounding sphere in this subtree from a_from.
	// Culled branches are skipped, so a shield hidden for the clash is not
	// measured.
	void Extend(RE::NiAVObject* a_object, const RE::NiPoint3& a_from, float& a_reach, RE::NiPoint3& a_farthest)
	{
		if (!a_object || a_object->GetAppCulled()) {
			return;
		}

		if (const auto geometry = a_object->AsGeometry()) {
			const auto& bound = geometry->GetModelData().modelBound;
			if (bound.radius > 0.0f) {
				const auto  center = geometry->world * bound.center;
				const float radius = bound.radius * std::abs(geometry->world.scale);
				auto        offset = center - a_from;
				const float length = offset.Length();
				const float reach = length + radius;
				if (reach > a_reach && length > 1.0f) {
					a_reach = reach;
					a_farthest = a_from + offset * (reach / length);
				}
			}
		}

		if (const auto node = a_object->AsNode()) {
			for (const auto& child : node->GetChildren()) {
				Extend(child.get(), a_from, a_reach, a_farthest);
			}
		}
	}

	// Catches a skeleton whose world transforms are not current (the player's
	// third-person model in first person, an actor just moved): a hand is
	// within arm's reach of its owner, a stale transform usually is not.
	[[nodiscard]] bool IsPlausibleHand(RE::Actor* a_actor, const RE::NiPoint3& a_grip)
	{
		const float scale = std::max(1.0f, a_actor->GetScale());
		const auto  offset = a_grip - a_actor->GetPosition();
		const float horizontal = std::sqrt(offset.x * offset.x + offset.y * offset.y);
		return horizontal <= 150.0f * scale && offset.z >= -50.0f * scale && offset.z <= 250.0f * scale;
	}

	// Only a weapon or shield is worth measuring; a spell effect node or a
	// torch is not what the other blade will meet.
	[[nodiscard]] bool HoldsSomethingSolid(RE::Actor* a_actor, bool a_leftHand)
	{
		const auto form = a_actor->GetEquippedObject(a_leftHand);
		if (!form) {
			return false;
		}
		if (form->As<RE::TESObjectWEAP>()) {
			return true;
		}
		const auto armor = form->As<RE::TESObjectARMO>();
		return armor && armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
	}
}

namespace BladeGeometry
{
	Blade Measure(RE::Actor* a_actor, const RE::NiPoint3& a_opponent)
	{
		Blade best;
		if (!a_actor) {
			return best;
		}
		const auto root = a_actor->Get3D(false);
		if (!root) {
			return best;
		}

		float bestToOpponent = std::numeric_limits<float>::max();
		for (const bool leftHand : { false, true }) {
			if (!HoldsSomethingSolid(a_actor, leftHand)) {
				continue;
			}
			const auto node = root->GetObjectByName(leftHand ? "SHIELD" : "WEAPON");
			if (!node || node->GetAppCulled()) {
				continue;
			}

			const auto grip = node->world.translate;
			if (!IsPlausibleHand(a_actor, grip)) {
				logger::debug("Blade: {}'s {} node is {:.0f} units from them; transforms are not current, not measured",
					a_actor->GetName(), leftHand ? "SHIELD" : "WEAPON", (grip - a_actor->GetPosition()).Length());
				continue;
			}
			float        reach = 0.0f;
			RE::NiPoint3 farthest = grip;
			Extend(node, grip, reach, farthest);
			if (reach < kMinBladeLength) {
				continue;
			}
			if (reach > kMaxBladeLength) {
				auto direction = farthest - grip;
				direction.Unitize();
				farthest = grip + direction * kMaxBladeLength;
			}

			// The nearer hand is the one that meets their weapon.
			const float toOpponent = (farthest - a_opponent).Length();
			if (toOpponent < bestToOpponent) {
				bestToOpponent = toOpponent;
				best.valid = true;
				best.grip = grip;
				best.tip = farthest;
				best.leftHand = leftHand;
			}
		}

		return best;
	}

	// Closest points of two segments (Ericson, Real-Time Collision Detection,
	// 5.1.9): clamped solve of the two parameters, degenerate segments
	// included.
	float ClosestPoints(const RE::NiPoint3& a_fromA, const RE::NiPoint3& a_toA, const RE::NiPoint3& a_fromB, const RE::NiPoint3& a_toB,
		RE::NiPoint3& a_onA, RE::NiPoint3& a_onB)
	{
		constexpr float kEpsilon = 1e-5f;

		const auto  d1 = a_toA - a_fromA;
		const auto  d2 = a_toB - a_fromB;
		const auto  r = a_fromA - a_fromB;
		const float len1 = d1.SqrLength();
		const float len2 = d2.SqrLength();
		const float f = d2.Dot(r);

		float s = 0.0f;
		float t = 0.0f;
		if (len1 <= kEpsilon && len2 <= kEpsilon) {
			// Both degenerate: the two starting points.
		} else if (len1 <= kEpsilon) {
			t = std::clamp(f / len2, 0.0f, 1.0f);
		} else {
			const float c = d1.Dot(r);
			if (len2 <= kEpsilon) {
				s = std::clamp(-c / len1, 0.0f, 1.0f);
			} else {
				const float b = d1.Dot(d2);
				const float denom = len1 * len2 - b * b;
				s = denom > kEpsilon ? std::clamp((b * f - c * len2) / denom, 0.0f, 1.0f) : 0.0f;
				t = b * s + f;
				if (t < 0.0f) {
					t = 0.0f;
					s = std::clamp(-c / len1, 0.0f, 1.0f);
				} else if (t > len2) {
					t = 1.0f;
					s = std::clamp((b - c) / len1, 0.0f, 1.0f);
				} else {
					t /= len2;
				}
			}
		}

		a_onA = a_fromA + d1 * s;
		a_onB = a_fromB + d2 * t;
		return (a_onA - a_onB).Length();
	}

	float Gap(const Blade& a_lhs, const Blade& a_rhs, RE::NiPoint3& a_onLhs, RE::NiPoint3& a_onRhs)
	{
		return ClosestPoints(a_lhs.grip, a_lhs.tip, a_rhs.grip, a_rhs.tip, a_onLhs, a_onRhs);
	}

	// One capsule, hips to head. Coarse on purpose: it keeps blades out of
	// chests, and a shoulder grazed by a couple of units is not noticed.
	Body MeasureBody(RE::Actor* a_actor, float a_radius)
	{
		Body body;
		if (!a_actor) {
			return body;
		}
		const auto root = a_actor->Get3D(false);
		if (!root) {
			return body;
		}

		constexpr const char* kLowNodes[] = { "NPC Pelvis [Pelv]", "NPC Spine [Spn0]", "NPC Root [Root]" };
		constexpr const char* kHighNodes[] = { "NPC Head [Head]", "NPC Neck [Neck]", "NPC Spine2 [Spn2]" };

		const auto find = [&](std::span<const char* const> a_names, RE::NiPoint3& a_out) {
			for (const auto name : a_names) {
				if (const auto node = root->GetObjectByName(name)) {
					a_out = node->world.translate;
					return true;
				}
			}
			return false;
		};

		if (!find(kLowNodes, body.low) || !find(kHighNodes, body.high)) {
			return body;
		}
		body.radius = a_radius * std::max(0.1f, a_actor->GetScale());
		body.valid = body.radius > 0.0f;
		return body;
	}

	float Penetration(const Blade& a_blade, const Body& a_body, RE::NiPoint3& a_onBlade, RE::NiPoint3& a_push)
	{
		a_push = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
		if (!a_blade.valid || !a_body.valid) {
			return 0.0f;
		}

		RE::NiPoint3 onBody;
		const float  distance = ClosestPoints(a_blade.grip, a_blade.tip, a_body.low, a_body.high, a_onBlade, onBody);

		// Straight out from the axis is the shortest way out. A blade lying on
		// the axis has no such direction, so it follows the hand instead.
		a_push = a_onBlade - onBody;
		float length = a_push.Length();
		if (length < 1.0f) {
			a_push = a_blade.grip - onBody;
			a_push.z = 0.0f;
			length = a_push.Length();
			if (length < 1.0f) {
				return 0.0f;
			}
		}
		a_push /= length;
		return a_body.radius - distance;
	}
}
