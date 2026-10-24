#pragma once
// STUB world for compiling + behavior-testing ogfn-brain-adapter.cpp OFF the engine.
// Shadow-header trick: the adapter source is copied into this directory before
// compiling, so its quoted includes resolve HERE instead of the real fork tree.
// Every type/method mirrors the fork's API surface exactly as the adapter uses it;
// anything the adapter calls that is not here is a compile error we WANT to see.

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <format>

using __int64 = long long; // MSVC name the adapter casts through

// Format-checked no-op: std::format validates every placeholder/arg like MSVC will.
#define LOG_INFO(cat, ...)  ((void)std::printf("%s\n", std::format(__VA_ARGS__).c_str()))
#define LOG_ERROR(cat, ...) ((void)std::printf("%s\n", std::format(__VA_ARGS__).c_str()))

// Sim clock, referenced by recorders defined before UWorld exists.
namespace Harness { extern double g_now; }
inline double HarnessNow() { return Harness::g_now; }

// MSVC CRT call the adapter uses on the real build; shim it for glibc here.
inline int DeleteFileA(const char*) { return 1; } // Win32 DeleteFile, shimmed
inline int fopen_s(FILE** out, const char* name, const char* mode)
{
	*out = std::fopen(name, mode);
	return *out ? 0 : 1;
}

struct FVector
{
	float X, Y, Z;
	FVector() : X(0), Y(0), Z(0) {}
	FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
	FVector operator+(const FVector& A) const { return { X + A.X, Y + A.Y, Z + A.Z }; }
	FVector operator-(const FVector& A) const { return { X - A.X, Y - A.Y, Z - A.Z }; }
};

struct FQuat { float X, Y, Z, W; FQuat(float x = 0, float y = 0, float z = 0, float w = 1) : X(x), Y(y), Z(z), W(w) {} };
struct FRotator { float Pitch, Yaw, Roll; FQuat Quaternion() const { return FQuat(); } };
struct FTransform { FQuat Rotation; FVector Translation; FVector Scale3D; };
struct FLinearColor { float R = 0, G = 0, B = 0, A = 0; };
struct FGuid { int v = 0; };

struct FHitResult
{
	bool bBlocking = false;
	FVector Location;                      // ground point for down-probes
	bool IsBlockingHit() const { return bBlocking; }
	FVector& GetLocation() { return Location; }
};

enum class ETraceTypeQuery { TraceTypeQuery1, TraceTypeQuery2 };
enum class EDrawDebugTrace { None, ForDuration };

namespace Harness { extern bool g_wallUp; extern float g_wallX; } // sim-defined
struct UFunction { std::string name; size_t paramsSize = 64; size_t GetPropertiesSize() const { return paramsSize; } };
// Real NameTypes.h FName; the stub has always used std::string for names, so an alias
// keeps FindObject/FindFunction call sites identical while giving FGameplayTag the same
// one-field shape the fork has (GameplayTagContainer.h).
struct FName { std::string name; };
// Real GameplayTagContainer.h: FGameplayTag is exactly one FName. It must occupy its slot
// in the ForceKill param struct even though early builds ignore it.
struct FGameplayTag { FName TagName; };
struct UClass { std::string name; };

// Fork's offset finder (reboot.h). Layout constants match the adapter's
// NewLocation-at-0 / bSweep-at-12 expectation for K2_SetActorLocation.
inline int FindOffsetStruct(const std::string&, const std::string& Member, bool = true)
{
	if (Member == "NewLocation") return 0;
	if (Member == "bSweep") return 12;
	if (Member == "ReturnValue") return 60; // stub UFunction paramsSize = 64
	return -1;
}

struct UObject
{
	std::string name = "UObject";
	std::map<std::string, int> offsets;          // GetOffset, missing = -1
	std::map<int, float> floatSlots;             // Get<float>
	std::map<int, FVector> vecSlots;             // Get<FVector>
	std::map<int, UObject*> ptrSlots;            // Get<UObject*>
	std::map<std::string, UFunction*> functions; // FindFunction

	virtual ~UObject() = default;
	virtual std::string GetPathName() const { return name; }
	bool IsValidLowLevel() { return true; } // sim objects are always allocated

	// The fork has both 1-arg and 2-arg forms; the default covers both call styles.
	int GetOffset(const std::string& n, bool bWarn = true) const
	{
		auto it = offsets.find(n);
		return it == offsets.end() ? -1 : it->second;
	}

	template <typename T>
	T Get(int off) const
	{
		if constexpr (std::is_same_v<T, float>)
		{
			auto it = floatSlots.find(off);
			return it == floatSlots.end() ? 0.f : it->second;
		}
		else if constexpr (std::is_same_v<T, UObject*>)
		{
			auto it = ptrSlots.find(off);
			return it == ptrSlots.end() ? nullptr : it->second;
		}
		else if constexpr (std::is_same_v<T, FVector>)
		{
			auto it = vecSlots.find(off);
			return it == vecSlots.end() ? FVector() : it->second;
		}
		else if constexpr (std::is_pointer_v<T>)
		{
			auto it = ptrSlots.find(off);
			return it == ptrSlots.end() ? nullptr : reinterpret_cast<T>(it->second);
		}
		else
		{
			static_assert(sizeof(T) == 0, "stub Get<T>: only the types the adapter uses are modeled");
		}
	}

	// Raw struct-at-offset pointer (fork's GetPtr<T>): generic by design.
	template <typename T>
	T* GetPtr(int off)
	{
		static char s_buf[256] = {};
		return reinterpret_cast<T*>(s_buf);
	}

	virtual UFunction* FindFunction(const std::string& n)
	{
		auto it = functions.find(n);
		return it == functions.end() ? nullptr : it->second;
	}

	// Event sink: named fns route to counters recorded per subclass (see AActor/Pawn stubs).
	virtual void ProcessEvent(UFunction* fn, void* params);
};

template <typename T>
T* Cast(UObject* o) { return dynamic_cast<T*>(o); }

struct AActor : UObject
{
	FVector Location;
	FRotator Rotation;
	bool bDestroyed = false;

	FVector GetActorLocation() { return Location; }
	FRotator GetActorRotation() { return Rotation; }
	// Real AActor (Actor.h:38, Actor.h:49). Needed because the adapter makes a spawned
	// building replicate; a stub without them would have let code that never touches the
	// network layer look fine, which is the failure mode this harness exists to avoid.
	int flushNetDormancyCount = 0;
	int forceNetUpdateCount = 0;
	void FlushNetDormancy() { flushNetDormancyCount++; }
	void ForceNetUpdate() { forceNetUpdateCount++; }
	bool IsActorBeingDestroyed() const { return bDestroyed; }
};

// Recording hooks -- the SIM's observable evidence.
struct ProcessEventRecorder
{
	int rotationCount = 0;
	int moveCount = 0;
	int jumpCount = 0;
	int fireCount = 0;
	int forceKillCount = 0;
	int teleportCount = 0;
	FVector lastTeleport;
	FRotator lastRotation;
	FVector lastMoveDir;
	std::vector<double> fireTimes;
};

inline void UObject::ProcessEvent(UFunction* fn, void* params)
{
	if (!fn)
		return;
	// default: subclasses record by fn name (wired in sim_main via AActor override)
}

// Real Controller.h: class AController : public AActor. ForceKill takes one, so the stub
// needs the type to exist rather than the adapter being written around its absence.
struct AController : AActor {};

// --- hierarchy the adapter walks ---
struct AFortPawn : AActor {};
struct AFortPickup;
// Real FortPlayerPawn.h:31 (AFortPlayerPawn : AFortPawn) and :52. The pickup RPC lives
// here, not on the pickup, because it is the PAWN that takes the item.
struct AFortPlayerPawn : AFortPawn
{
	static inline int s_handleCalls = 0;
	static void ServerHandlePickupHook(AFortPlayerPawn* Pawn, AFortPickup* Pickup,
	                                   float InFlyTime, FVector InStartDirection,
	                                   bool bPlayPickupSound);
};
struct AFortPlayerPawnAthena : AFortPlayerPawn, ProcessEventRecorder
{
	static inline std::vector<AFortPlayerPawnAthena*> s_pawns;
	AFortPlayerPawnAthena() { s_pawns.push_back(this); }
	float Health = 100.f, Shield = 100.f;
	float GetHealth() { return Health; }
	FVector GetActorLocation() { return Location; }
	float GetShield() { return Shield; }
	// Present on the real AFortPawn (FortPawn.h: SetHealth/SetShield/SetMaxShield).
	// A stub without them does not mirror the client, it merely agrees with whatever
	// the adapter happens to call.
	void SetHealth(float v) { Health = v; }
	void SetShield(float v) { Shield = v; }
	void SetMaxHealth(float v) { MaxHealth = v; }
	void SetMaxShield(float v) { MaxShield = v; }
	float MaxHealth = 100.f, MaxShield = 100.f;
	bool bDBNO = false;
	bool IsDBNO() { return bDBNO; }
	void SetDBNO(bool v) { bDBNO = v; }
	void ProcessEvent(UFunction* fn, void* params) override
	{
		if (!fn) return;
		if (fn->name == "SetActorRotation" && params) { rotationCount++; lastRotation = ((FRotator*)params)[0]; Rotation = lastRotation; }
		else if (fn->name == "AddMovementInput" && params) { moveCount++; lastMoveDir = ((FVector*)params)[0]; }
		else if (fn->name == "Jump") jumpCount++;
		else if (fn->name == "StartFire") { fireCount++; fireTimes.push_back(HarnessNow()); }
		// A real elimination. Recorded separately from "health reached 0" because on the
		// live build those are different things: health at 0 with no ForceKill leaves the
		// bot standing and the match reports elim=0.
		else if (fn->name == "ForceKill") { forceKillCount++; }
		else if (fn->name == "K2_SetActorLocation" && params)
		{
			teleportCount++;
			FVector Dest = ((FVector*)params)[0]; // NewLocation at offset 0
			bool bBlocked = false;
			// Wall plane (like the sim's LOS wall): a sweep that would cross it
			// is blocked, so bots must path around it, not through it.
			if (Harness::g_wallUp && (Location.X - Harness::g_wallX) * (Dest.X - Harness::g_wallX) < 0.f)
				bBlocked = true;
			// Pawn-capsule collision, like a real bSweep=true sweep: any other
			// pawn within ~100uu horizontally and 200uu vertically stops us.
			for (auto* Other : s_pawns)
			{
				if (Other == this || Other->bDestroyed)
					continue;
				float dx = Other->Location.X - Dest.X;
				float dy = Other->Location.Y - Dest.Y;
				float dz = Other->Location.Z - Dest.Z;
				if (dx * dx + dy * dy < 10000.f && dz > -200.f && dz < 200.f)
				{
					bBlocked = true;
					break;
				}
			}
			if (!bBlocked)
			{
				lastTeleport = Dest;
				Location = Dest;
			}
			((unsigned char*)params)[60] = bBlocked ? 0 : 1; // ReturnValue: moved
		}
	}
};

struct UFortItemDefinition;
struct FFortItemEntry
{
	FGuid Guid;
	UFortItemDefinition* Definition = nullptr;
	int Count = 1;
	FGuid& GetItemGuid() { return Guid; }
	UFortItemDefinition*& GetItemDefinition() { return Definition; }
	int& GetCount() { return Count; }
};
struct UFortItem : UObject { FFortItemEntry Entry; FFortItemEntry* GetItemEntry() { return &Entry; } };

// Mirrors the REAL fork's shape, which is the whole point of a stub. An earlier
// version of this stub invented a flat `std::vector<UFortItem*> Items`; the adapter
// was written against that invention, the harness passed, and the actual MSVC build
// failed with "Items is not a member of AFortInventory". The real access is
// GetItemList().GetItemInstances() (FortInventory.h), so the stub now says that and
// the harness cannot certify code the client will reject.
// The REAL fork's TArray has no begin()/end(), only Num() and At() (Array.h), so a
// range-for over it does not compile. This stub used std::vector and therefore
// certified a range-for that the client rejects. It now exposes the same Num()/At()
// surface, so anything that compiles here compiles there.
template <typename T>
struct TArrayStub
{
	std::vector<T> v;
	int Num() const { return static_cast<int>(v.size()); }
	T& At(int i) { return v[static_cast<size_t>(i)]; }
	void push_back(T x) { v.push_back(x); }
	size_t size() const { return v.size(); }
	T& operator[](size_t i) { return v[i]; }
};

struct FFortItemList
{
	TArrayStub<UFortItem*> ItemInstances;
	TArrayStub<UFortItem*>& GetItemInstances() { return ItemInstances; }
};

struct AFortInventory : UObject
{
	FFortItemList List;
	int adds = 0;
	int updates = 0;

	FFortItemList& GetItemList() { return List; }

	template <typename DefT>
	std::pair<std::vector<UFortItem*>, std::vector<UFortItem*>> AddItem(DefT* Def, bool* bShouldUpdate, int Count = 1, int = -1, bool = false)
	{
		adds++;
		if (bShouldUpdate) *bShouldUpdate = true;
		auto Item = new UFortItem();
		Item->name = "ItemInstance";
		// The real AddItem hands the instance the definition it was given. Dropping it
		// made every granted weapon unidentifiable by name, so anything that armed a bot
		// by re-reading the inventory (equip-the-best-you-carry) equipped nothing.
		Item->Entry.Definition = Def;
		Item->Entry.Count = Count;
		List.ItemInstances.push_back(Item);
		return { { Item }, {} };
	}
	void Update() { updates++; }
};

struct AFortPlayerController : UObject
{
	UObject* PS = nullptr;
	UObject* GetPlayerState() { return PS; }
	// Static on the base class, exactly where the real fork declares it
	// (FortPlayerController.h). The counter rides along for sim evidence.
	static inline int equipCalls = 0;
	static void ServerExecuteInventoryItemHook(AFortPlayerController*, FGuid)
	{
		equipCalls++;
	}

	// GetMyFortPawn is base-class in the real fork too (FortPlayerController.h),
	// which matters: the adapter calls it through a base pointer.
	AFortPawn* MyPawn = nullptr;
	AFortPawn*& GetMyFortPawn() { return MyPawn; }
	// AController::GetPawn in the real engine: the adapter reads the pawn FRESH
	// from the controller every tick (respawn replaces the cached pointer).
	AFortPawn* GetPawn() { return MyPawn; }
};
struct AFortPlayerControllerAthena : AFortPlayerController
{
	AFortInventory* Inventory = nullptr;

	AFortInventory* GetWorldInventory() { return Inventory; }
};

struct AFortPlayerState : UObject {};

// --- network/world shell ---
struct UNetConnection : UObject
{
	AFortPlayerController* PC = nullptr;
	AFortPlayerController* GetPlayerController() { return PC; }
};

template <typename T>
struct TArray
{
	std::vector<T> v;
	TArray() = default;
	std::initializer_list<T> il;
	TArray(std::initializer_list<T> l) : v(l) {}
	int Num() const { return (int)v.size(); }
	T At(int i) const { return v[(size_t)i]; }
	T at(int i) const { return v[(size_t)i]; }
};

struct UNetDriver
{
	TArray<UNetConnection*> Connections;
	TArray<UNetConnection*>& GetClientConnections() { return Connections; }
};

// Forward-declared so the templated SpawnActor below can be declared before the
// building types exist; the adapter includes BuildingSMActor-equivalent stubs.
struct ABuildingSMActorStubTag {};

struct UWorld : UObject
{
	using UObject::UObject;
	virtual UObject* GetGameState() { return nullptr; }
	virtual UNetDriver* GetNetDriver() { return nullptr; }
	virtual UObject* GetGameMode() { return nullptr; }

	// Real UWorld::SpawnActor<T>(Class, FTransform). The harness needs it because the
	// adapter's build path spawns a real actor; without it, that path was compiled out
	// under OGFN_HARNESS and its replication calls were never tested.
	static inline int s_spawnCalls = 0;
	template <typename T>
	T* SpawnActor(UClass* Cls, const FTransform& Xf)
	{
		if (!Cls) return nullptr;
		s_spawnCalls++;
		T* A = new T();
		A->Location = Xf.Translation;
		return A;
	}
};

namespace Harness
{
	extern UWorld* g_world;
	extern double g_now;
	extern std::map<std::wstring, UClass*> g_namedClasses;
	extern std::map<UClass*, std::vector<UObject*>> g_classObjects;
}

inline UWorld* GetWorld() { return Harness::g_world; }

template <typename T>
T* FindObject(const wchar_t* name)
{
	auto& M = Harness::g_namedClasses;
	auto it = M.find(name);
	return it == M.end() ? nullptr : dynamic_cast<T*>(it->second);
}

template <typename T>
std::vector<T*> GetAllObjectsOfClass(UClass* c)
{
	std::vector<T*> out;
	auto& M = Harness::g_classObjects;
	auto it = M.find(c);
	if (it != M.end())
		for (auto* o : it->second)
			if (auto* t = dynamic_cast<T*>(o))
				out.push_back(t);
	return out;
}

// Non-template overload (binds plain GetAllObjectsOfClass(x) calls, as the real fork has).
inline std::vector<UObject*> GetAllObjectsOfClass(UClass* c)
{
	return GetAllObjectsOfClass<UObject>(c);
}

struct UGameplayStatics
{
	static TArray<AActor*> GetAllActorsOfClass(UWorld*, UClass* c)
	{
		TArray<AActor*> out;
		auto& M = Harness::g_classObjects;
		auto it = M.find(c);
		if (it != M.end())
			for (auto* o : it->second)
				if (auto* a = dynamic_cast<AActor*>(o))
					out.v.push_back(a);
		return out;
	}
	static double GetTimeSeconds(UWorld*) { return Harness::g_now; }
};

struct UFortItemDefinition : UObject
{
	// The real Object.h resolves this through a name index; the sim just uses name.
	std::string GetFullName() const { return name; }
};
