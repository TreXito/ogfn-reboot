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

struct FRotator { float Pitch, Yaw, Roll; };
struct FLinearColor { float R = 0, G = 0, B = 0, A = 0; };
struct FGuid { int v = 0; };

struct FHitResult
{
	bool bBlocking = false;
	bool IsBlockingHit() const { return bBlocking; }
};

enum class ETraceTypeQuery { TraceTypeQuery1, TraceTypeQuery2 };
enum class EDrawDebugTrace { None, ForDuration };

struct UFunction { std::string name; size_t paramsSize = 64; size_t GetPropertiesSize() const { return paramsSize; } };
struct UClass { std::string name; };

struct UObject
{
	std::string name = "UObject";
	std::map<std::string, int> offsets;          // GetOffset, missing = -1
	std::map<int, float> floatSlots;             // Get<float>
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
		else
		{
			static_assert(sizeof(T) == 0, "stub Get<T>: only the types the adapter uses are modeled");
		}
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
	bool IsActorBeingDestroyed() const { return bDestroyed; }
};

// Recording hooks -- the SIM's observable evidence.
struct ProcessEventRecorder
{
	int rotationCount = 0;
	int moveCount = 0;
	int jumpCount = 0;
	int fireCount = 0;
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

// --- hierarchy the adapter walks ---
struct AFortPawn : AActor {};
struct AFortPlayerPawnAthena : AFortPawn, ProcessEventRecorder
{
	float Health = 100.f, Shield = 100.f;
	float GetHealth() { return Health; }
	float GetShield() { return Shield; }
	void ProcessEvent(UFunction* fn, void* params) override
	{
		if (!fn) return;
		if (fn->name == "SetActorRotation" && params) { rotationCount++; lastRotation = ((FRotator*)params)[0]; }
		else if (fn->name == "AddMovementInput" && params) { moveCount++; lastMoveDir = ((FVector*)params)[0]; }
		else if (fn->name == "Jump") jumpCount++;
		else if (fn->name == "StartFire") { fireCount++; fireTimes.push_back(HarnessNow()); }
	}
};

struct FFortItemEntry { FGuid Guid; FGuid& GetItemGuid() { return Guid; } };
struct UFortItem : UObject { FFortItemEntry Entry; FFortItemEntry* GetItemEntry() { return &Entry; } };

struct AFortInventory : UObject
{
	std::vector<UFortItem*> Items;
	int adds = 0;
	int updates = 0;

	template <typename DefT>
	std::pair<std::vector<UFortItem*>, std::vector<UFortItem*>> AddItem(DefT*, bool* bShouldUpdate, int = 1, int = -1, bool = false)
	{
		adds++;
		if (bShouldUpdate) *bShouldUpdate = true;
		auto Item = new UFortItem();
		Item->name = "ItemInstance";
		Items.push_back(Item);
		return { { Item }, {} };
	}
	void Update() { updates++; }
};

struct AFortPlayerController : UObject
{
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

struct UWorld : UObject
{
	using UObject::UObject;
	virtual UObject* GetGameState() { return nullptr; }
	virtual UNetDriver* GetNetDriver() { return nullptr; }
	virtual UObject* GetGameMode() { return nullptr; }
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

struct UFortItemDefinition : UObject {};
