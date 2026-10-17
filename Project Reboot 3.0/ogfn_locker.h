#pragma once

// OGFN Locker -- every cosmetic that exists in the loaded client build, no backend required.
// On 3.5.0 that is the full Season 3 catalogue: the object pool only contains what the client
// ships, so every skin listed is a skin that can actually be worn.
//
// Characters apply through the same proven path as the console command `applycid`
// (ApplyCID -> ApplyHID in FortPlayerControllerAthena.h); emotes through the same
// ServerPlayEmoteItemHook the bot tick already calls. No client profile is written -- without
// an MCP backend there is no profile to write into; this is the server applying cosmetics to
// the host pawn, which the client then renders. That is the honest whole-game "full locker".

#include "reboot.h"
#include "globals.h"
#include "UnrealString.h"
#include <cstdio>
#include "KismetTextLibrary.h"
#include "FortPlayerController.h"
#include "FortPlayerControllerAthena.h"

namespace OgfnLocker
{
	extern inline bool bPopulated = false;
	extern inline std::vector<UObject*> Characters = {}; // AthenaCharacterItemDefinition
	extern inline std::vector<UObject*> Emotes = {};     // AthenaDanceItemDefinition
	extern inline std::vector<std::string> CharacterNames = {};
	extern inline std::vector<std::string> EmoteNames = {};

	extern inline int SelectedCharacter = -1;
	extern inline int SelectedEmote = -1;
	extern inline char SearchBuf[64] = {};

	// Auto-apply on first joined player: the in-game client locker does not reach the
	// server (no MCP cosmetic plumbing on 3.5 without Reboot console access), so the
	// server dresses the pawn itself. The full CID catalog is dumped to disk once so
	// any preference can be wired by name later.
	extern inline bool bAutoApplied = false;

	// Fresh GetOffset per object: character and emote definitions are different classes and
	// must not share a cached offset.
	static std::string GetDisplayName(UObject* Object, const std::string& Property)
	{
		auto Offset = Object->GetOffset(Property);

		if (Offset == -1)
			return {};

		FString AsFString = UKismetTextLibrary::Conv_TextToString(Object->Get<FText>(Offset));

		if (!AsFString.Data.Data)
			return {};

		return AsFString.ToString();
	}

	static AFortPlayerControllerAthena* GetLocalController()
	{
		auto Connections = GetWorld()->GetNetDriver()->GetClientConnections();

		for (int i = 0; i < Connections.Num(); ++i)
		{
			auto PlayerController = (AFortPlayerControllerAthena*)Connections.At(i)->GetPlayerController();

			if (PlayerController)
				return PlayerController;
		}

		return nullptr;
	}

	static void Populate()
	{
		if (bPopulated)
			return;

		static auto AthenaCharacterItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaCharacterItemDefinition");
		static auto AthenaDanceItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaDanceItemDefinition");

		Characters = GetAllObjectsOfClass(AthenaCharacterItemDefinitionClass);
		Emotes = GetAllObjectsOfClass(AthenaDanceItemDefinitionClass);

		CharacterNames.reserve(Characters.size());

		for (auto& Character : Characters)
			CharacterNames.push_back(GetDisplayName(Character, "DisplayName"));

		EmoteNames.reserve(Emotes.size());

		for (auto& Emote : Emotes)
			EmoteNames.push_back(GetDisplayName(Emote, "DisplayName"));

		bPopulated = true;

		LOG_INFO(LogBots, "[ogfn] locker populated: {} characters, {} emotes.", Characters.size(), Emotes.size());
	}

	static void ApplyCharacter(int Index)
	{
		auto PlayerController = GetLocalController();

		if (!PlayerController)
		{
			LOG_ERROR(LogBots, "[ogfn] locker: no local player controller yet.");
			return;
		}

		auto Pawn = Cast<AFortPlayerPawn>(PlayerController->GetMyFortPawn());

		if (!Pawn)
		{
			LOG_ERROR(LogBots, "[ogfn] locker: no pawn yet -- join the lobby first.");
			return;
		}

		auto CID = Characters.at(Index);

		if (ApplyCID(Pawn, CID))
		{
			LOG_INFO(LogBots, "[ogfn] locker: applied {}.", CID->GetPathName());
		}
		else
		{
			LOG_ERROR(LogBots, "[ogfn] locker: ApplyCID failed for {}.", CID->GetPathName());
		}
	}

	// Called repeatedly from the ReadyToStartMatch hook: once a connected player has a
	// pawn, dress it once with a random catalog skin and dump the full CID list to
	// C:\ogfn\cidlist.txt so any skin can be pinned by name in a later iteration.
	// (The in-game client locker cannot reach the server: 3.5 has no MCP cosmetic
	// plumbing and Reboot's ServerCheat console route is disabled in this build.)
	static void AutoApplyOnce()
	{
		if (bAutoApplied)
			return;

		Populate();

		if (Characters.empty())
			return;

		if (!GetLocalController())
			return; // nobody joined yet

		static bool bDumped = false;
		if (!bDumped)
		{
			bDumped = true;
			FILE* f = nullptr;
			if (fopen_s(&f, "C:\\ogfn\\cidlist.txt", "w") == 0 && f)
			{
				for (int i = 0; i < (int)Characters.size(); ++i)
				{
					fprintf(f, "%s\t%s\n", Characters.at(i)->GetPathName().c_str(),
					        CharacterNames.at(i).c_str());
				}
				fclose(f);
			}
		}

		int idx = rand() % (int)Characters.size();
		LOG_INFO(LogBots, "[ogfn] auto-applying skin {} ({}/{})", Characters.at(idx)->GetPathName(),
		         idx + 1, (int)Characters.size());
		ApplyCharacter(idx);
		bAutoApplied = true;
	}

	static void PlayEmote(int Index)
	{
		auto PlayerController = GetLocalController();

		if (!PlayerController)
		{
			LOG_ERROR(LogBots, "[ogfn] locker: no local player controller yet.");
			return;
		}

		AFortPlayerController::ServerPlayEmoteItemHook(PlayerController, Emotes.at(Index));
	}

	static std::string LabelFor(const std::vector<std::string>& Names, UObject* Object, int Index)
	{
		const auto& Name = Names.at(Index);
		return (Name.empty() ? Object->GetPathName() : Name) + std::format("##{}", Index);
	}

	static bool MatchesSearch(const std::string& Label)
	{
		return !SearchBuf[0] || Label.find(SearchBuf) != std::string::npos;
	}

	static void Render()
	{
		Populate();

		if (!bPopulated || Characters.empty())
		{
			ImGui::TextUnformatted("No cosmetics found in the loaded client.");
			return;
		}

		if (ImGui::CollapsingHeader("Character (skin)"))
		{
			ImGui::InputText("Search", SearchBuf, sizeof(SearchBuf));

			ImGui::BeginChild("##ogfn_characters", ImVec2(0, 240), true);

			for (int i = 0; i < (int)Characters.size(); ++i)
			{
				auto Label = LabelFor(CharacterNames, Characters.at(i), i);

				if (!MatchesSearch(Label))
					continue;

				if (ImGui::Selectable(Label.c_str(), SelectedCharacter == i))
					SelectedCharacter = i;
			}

			ImGui::EndChild();

			if (SelectedCharacter != -1 && ImGui::Button("Apply selected"))
				ApplyCharacter(SelectedCharacter);

			ImGui::SameLine();

			if (ImGui::Button("Random"))
				ApplyCharacter(std::rand() % (int)Characters.size());
		}

		if (ImGui::CollapsingHeader("Emote"))
		{
			const char* Preview = "<pick>";

			static std::string PreviewBuffer;

			if (SelectedEmote != -1)
			{
				PreviewBuffer = EmoteNames.at(SelectedEmote).empty()
					? Emotes.at(SelectedEmote)->GetPathName()
					: EmoteNames.at(SelectedEmote);
				Preview = PreviewBuffer.c_str();
			}

			if (ImGui::BeginCombo("##ogfn_emote", Preview))
			{
				for (int i = 0; i < (int)Emotes.size(); ++i)
				{
					auto Label = LabelFor(EmoteNames, Emotes.at(i), i);

					bool bIsSelected = (SelectedEmote == i);

					if (ImGui::Selectable(Label.c_str(), bIsSelected))
						SelectedEmote = i;

					if (bIsSelected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}

			if (SelectedEmote != -1)
			{
				if (ImGui::Button("Play"))
					PlayEmote(SelectedEmote);

				ImGui::SameLine();

				if (ImGui::Button("Random"))
					PlayEmote(std::rand() % (int)Emotes.size());
			}
		}

		ImGui::TextUnformatted("Full locker: everything the loaded client ships with. Pick and apply while in lobby or match.");
	}
}
