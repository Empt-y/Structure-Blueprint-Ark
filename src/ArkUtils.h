#pragma once

// Small helpers shared by the plugin. Nothing here is specific to blueprints;
// it is the glue between ArkApi's raw field accessors and normal C++ types.

#include <API/ARK/Ark.h>
#include <string>
#include <cmath>

namespace sb
{
	// Canonical ArkApi blueprint-path helper.
	//
	// GetFullName() on a class default object yields something like
	//   "BlueprintGeneratedClass /Game/.../Foundation_Wood.Default__Foundation_Wood_C"
	// but UVictoryCore::BPLoadClass expects
	//   "Blueprint'/Game/.../Foundation_Wood.Foundation_Wood'"
	// so we strip the leading type token, drop the trailing "_C", and remove the
	// "Default__" prefix that GetDefaultObject introduces.
	inline FString GetBlueprintPath(UObjectBase* object)
	{
		if (object == nullptr || object->ClassField() == nullptr)
			return FString("");

		FString path_name;
		object->ClassField()->GetDefaultObject(true)->GetFullName(&path_name, nullptr);

		int space_index = 0;
		if (!path_name.FindChar(' ', space_index))
			return FString("");

		path_name = path_name.Mid(space_index + 1, path_name.Len() - (space_index + 1));
		path_name = "Blueprint'" + path_name.Left(path_name.Len() - 2) + "'";
		return path_name.Replace(L"Default__", L"");
	}

	inline std::string ToStd(const FString& s) { return s.ToString(); }

	// Structures keep their root component unattached, so relative == world here.
	inline bool ReadTransform(AActor* actor, FVector& out_loc, FRotator& out_rot)
	{
		if (actor == nullptr) return false;
		USceneComponent* root = actor->RootComponentField();
		if (root == nullptr) return false;
		out_loc = root->RelativeLocationField();
		out_rot = root->RelativeRotationField();
		return true;
	}

	// Rotate an offset around the vertical axis. Paste rotation is applied to the
	// whole build about its origin, so each piece's offset and yaw both rotate.
	inline FVector RotateAroundZ(const FVector& v, float yaw_degrees)
	{
		const double rad = static_cast<double>(yaw_degrees) * 3.14159265358979323846 / 180.0;
		const double c = std::cos(rad);
		const double s = std::sin(rad);
		FVector out;
		out.X = static_cast<float>(v.X * c - v.Y * s);
		out.Y = static_cast<float>(v.X * s + v.Y * c);
		out.Z = v.Z;
		return out;
	}

	// Class-based tests, not name heuristics: reading a container or turret field
	// off a structure that is neither returns whatever sits at that offset.
	// Resolved once - FindClass walks the whole object array.
	inline UClass* ContainerClass()
	{
		static UClass* c = Globals::FindClass("Class /Script/ShooterGame.PrimalStructureItemContainer");
		return c;
	}

	inline UClass* TurretClass()
	{
		static UClass* c = Globals::FindClass("Class /Script/ShooterGame.PrimalStructureTurret");
		return c;
	}

	inline bool IsA(AActor* actor, UClass* base)
	{
		return actor != nullptr && base != nullptr
			&& actor->ClassField() != nullptr && actor->ClassField()->IsChildOf(base);
	}

	inline bool IsAdmin(AShooterPlayerController* pc)
	{
		return pc != nullptr && pc->bIsAdmin()();
	}

	inline void Reply(AShooterPlayerController* pc, const std::wstring& msg)
	{
		if (pc == nullptr) return;
		ArkApi::GetApiUtils().SendChatMessage(pc, L"StructureBlueprint", msg.c_str());
	}
} // namespace sb
