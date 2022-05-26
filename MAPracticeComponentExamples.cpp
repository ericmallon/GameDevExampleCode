/**

A small selection of functionality from code handling practice mode.
This mode allows players to record movement paths, play them back, spawn bots, 
set up and play practice drills with a variety of victory conditions, and more.

*/
//Called from practice menu blueprint widget, stores all practice mode data to a json file to persist it.
void UMAPracticeComponent::SaveAllPracticeDataToFile()
{
	if (!IsPracticeModeCommandEnabled())
	{
		return;
	}
	FMAMapPracticeData MapPracticeDataToSave;
	//Currently we just allow a single map per file, could improve this later to allow multiple maps.
	MapPracticeDataToSave.MapName = GetWorld()->GetMapName();
	MapPracticeDataToSave.RouteTrails = RouteTrails;
	MapPracticeDataToSave.Drills = Drills;
	MapPracticeDataToSave.Bots = MapPracticeData.Bots;
	MapPracticeDataToSave.Locations = MapPracticeData.Locations;
	MapPracticeDataToSave.Author = ParentController->PlayerState->GetPlayerName();
	MapPracticeDataToSave.Tutorials = MapPracticeData.Tutorials;
	FString JSONPracticeData = "";

	FJsonObjectConverter::UStructToJsonObjectString(MapPracticeDataToSave, JSONPracticeData);

	/**
	* Upload practice data to the API
	*/
	//UMAGameGlobals::Get().ServicesAPI.CreatePracticeMap(MapPracticeDataToSave, [this](bool bSuccess, FMAMapPracticeData NewPracticeMap)
	//{
	//	return bSuccess;
	//});

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		void* ParentWindowHandle = GEngine->GameViewport->GetWindow()->GetNativeWindow()->GetOSWindowHandle();
		FString SaveDirectory = FPaths::Combine(FPaths::GameContentDir(), TEXT("/Practice"));
		FString FileTypes;
		FString DefaultFileName = FString("MidairPracticeData-").Append(MapPracticeDataToSave.MapName).Append(FString(".txt"));
		TArray<FString> OutFileNames;
		DesktopPlatform->SaveFileDialog(ParentWindowHandle, FString("Save Practice Data File"), SaveDirectory, DefaultFileName, FileTypes, 0, OutFileNames);
		if (OutFileNames.Num() > 0)
		{
			FString FileName = OutFileNames[0];
			FString TextToSave = JSONPracticeData;
			bool AllowOverwriting = false;

			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

			// CreateDirectoryTree returns true if the destination
			// directory existed prior to call or has been created
			// during the call.
			if (PlatformFile.CreateDirectoryTree(*SaveDirectory))
			{
				// Get absolute file path
				FString AbsoluteFilePath = SaveDirectory + "/" + FileName;
				FFileHelper::SaveStringToFile(TextToSave, *AbsoluteFilePath);
			}
		}
	}
}



void UMAPracticeComponent::StartSelectedDrillOrTutorial(bool bIsTutorial)
{
	if (!IsPracticeModeCommandEnabled() || SelectedDrill.Name.Equals("") || SelectedDrill.Name.IsEmpty())
	{
		return;
	}
	DrillResultMessage = "";
	DrillKillCounter = 0;
	DrillMidairCounter = 0;
	bIsActiveSpeedDrill = SelectedDrill.VictoryType == EDrillVictoryType::MovementSpeed;

	if (!SelectedDrill.LeaveOldBots)
	{
		KillAllBots();
	}


	//delete any previously spawned victory locations.
	if (IsValid(SpawnedDrillVictoryLocation) && SpawnedDrillVictoryLocation->IsPendingKillPending() == false && SpawnedDrillVictoryLocation->IsPendingKill() == false) {
		SpawnedDrillVictoryLocation->Destroy();
		SpawnedDrillVictoryLocation = nullptr;
	}

	//teleport player to drill/tutorial start location, if one is configured
	FPlayerLocationAndState PlayerSpawnLocation = SelectedDrill.InitialPlayerNamedLocation.LocationAndState;
	if (FMath::Abs(PlayerSpawnLocation.Location.X) > KINDA_SMALL_NUMBER || FMath::Abs(PlayerSpawnLocation.Location.Z) > KINDA_SMALL_NUMBER)
	{
		if (AMAPlayerState* PS = Cast<AMAPlayerState>(ParentController->PlayerState))
		{
			if (SelectedDrill.InitialPlayerNamedLocation.LocationTeam != PS->GetTeamId())
			{
				bool bRotationallyMirrored = IsCurrentMapRotationallyMirrored();
				FPlayerLocationAndState MirroredLocation = SwapPlayerLocationAndStateTeam(PlayerSpawnLocation, bRotationallyMirrored);
				LoadPosition(MirroredLocation, true);

			}
			else {
				LoadPosition(PlayerSpawnLocation, true);
			}

		}
	}
	if (SelectedDrill.DrillLength > 0.0f)
	{
		ParentController->GetWorldTimerManager().SetTimer(TimerHandle_DrillLength, this, &UMAPracticeComponent::EndCurrentDrillByTimeout, SelectedDrill.DrillLength, true, SelectedDrill.DrillLength);
	}
	else {
		ParentController->GetWorldTimerManager().SetTimer(TimerHandle_DrillLength, this, &UMAPracticeComponent::EndCurrentDrillByTimeout, 9999.0f, true, 9999.0f);
	}
	if (SelectedDrill.ResetFlagsOnStart)
	{
		ResetFlags();
	}
	//First, choose which routes of the loaded routes we are going to run bots on
	TArray<int> RoutesToRun;
	int BotsToSpawn = SelectedDrill.NumberOfBots;

	//drill struct contains just bot names, so pull down the full bot object and fetch the routes they know
	TArray<FMABotConfig> BotsForDrill;
	TSet<FString> RoutesBotsKnow;
	for (FMABotConfig BotConfig : MapPracticeData.Bots)
	{
		if (SelectedDrill.BotNames.Contains(BotConfig.Name))
		{
			BotsForDrill.Add(BotConfig);
			for (FString RouteTrailName : BotConfig.RouteTrailNames)
			{
				RoutesBotsKnow.Add(RouteTrailName);
			}

		}
	}
	if (SelectedDrill.BotsSpawnOnDifferentRoutes && BotsToSpawn > RoutesBotsKnow.Num())
	{
		BotsToSpawn = RoutesBotsKnow.Num();
	}
	if (!SelectedDrill.CanRepeatBots && SelectedDrill.NumberOfBots > SelectedDrill.BotNames.Num())
	{
		BotsToSpawn = FMath::Min(BotsToSpawn, SelectedDrill.BotNames.Num());
	}
	//add random bots to the drill from those allowable, preventing duplicates if that flag is set in the drill setup
	TArray<FMABotConfig> BotsToSpawnForDrill;
	for (int i = 0; i < BotsToSpawn; i++) {
		int BotIndex = FMath::RandRange(0, BotsForDrill.Num() - 1);
		BotsToSpawnForDrill.Add(BotsForDrill[BotIndex]);
		if (!SelectedDrill.CanRepeatBots)
		{
			BotsForDrill.RemoveAt(BotIndex);
		}
	}

	//if there is an end location marked, spawn it
	if (!SelectedDrill.VictoryLocation.Name.IsEmpty())
	{
		FMANamedLocation VictoryLoc = SelectedDrill.VictoryLocation;
		if (AMAPlayerState* PS = Cast<AMAPlayerState>(ParentController->PlayerState))
		{
			//If we are on the opposite team of the location, mirror the position around the origin to get the corresponding location for the other team
			if (PS->GetTeamId() != VictoryLoc.LocationTeam) {
				VictoryLoc.LocationAndState = SwapPlayerLocationAndStateTeam(VictoryLoc.LocationAndState, IsCurrentMapRotationallyMirrored());
			}
		}

		ADrillVictoryLocation* VictoryLocation = GetControlledCharacter()->GetWorld()->SpawnActor<ADrillVictoryLocation>(DrillVictoryLocationBluePrintClass,
			VictoryLoc.LocationAndState.Location, VictoryLoc.LocationAndState.Rotation);
		VictoryLocation->LocationAndState = VictoryLoc.LocationAndState;
		VictoryLocation->SetSize(SelectedDrill.VictoryLocationRadius, SelectedDrill.VictoryLocationHalfHeight);
		SpawnedDrillVictoryLocation = VictoryLocation;
	}


	//Then, start any routes we can start immediately:
	for (FMABotConfig Bot : BotsToSpawnForDrill)
	{
		ServerSpawnBot(Bot);
	}

}

void UMAPracticeComponent::EndCurrentDrillByTimeout()
{
	bool bDrillWon = false;

	//on drill end time being hit, we could still win a NoFlagCarrier type drill, since that is the point of the drill -- no carrier by timeout
	if (SelectedDrill.VictoryType == EDrillVictoryType::NoFlagCarrier)
	{
		bDrillWon = true;
		//For NoFlagCarrier, we just loop through the flag actors and see if they are being held by a bot. If they are, we lose. Otherwise, win.
		for (TActorIterator<AMACTFFlag> ActorItr(GetWorld()); ActorItr; ++ActorItr)
		{
			// Same as with the Object Iterator, access the subclass instance with the * or -> operators.
			AMACTFFlag *Flag = *ActorItr;
			if (Flag->Holder != nullptr)
			{
				if (AMAPlayerState* AIPS = Cast<AMAPlayerState>(Flag->Holder->PlayerState))
				{
					AMAPlayerState* PS = Cast<AMAPlayerState>(ParentController->PlayerState);
					if (AIPS->GetTeamId() != PS->GetTeamId())
					{
						EndCurrentDrill(false);
						return;
					}
				}
			}
		}
	}
	EndCurrentDrill(bDrillWon);
}

void UMAPracticeComponent::EndCurrentDrill(bool bDrillWon)
{
	ParentController->GetWorldTimerManager().ClearTimer(TimerHandle_DrillLength);
	if (!SelectedDrill.LeaveOldBots)
	{
		KillAllBots();
	}

	bIsActiveSpeedDrill = false;

	if (IsValid(SpawnedDrillVictoryLocation) && SpawnedDrillVictoryLocation->IsPendingKillPending() == false && SpawnedDrillVictoryLocation->IsPendingKill() == false) {
		SpawnedDrillVictoryLocation->Destroy();
		SpawnedDrillVictoryLocation = nullptr;
	}


	if (bDrillWon)
	{
		DrillResultMessage = TEXT("Drill Completed!");
		if (!bIsDrillRunningAsTutorial && !bIsDrillRunningAsWatcher)
		{
			DrillVictories++;
		}
	}
	else
	{
		if (!bIsDrillRunningAsTutorial && !bIsDrillRunningAsWatcher)
		{
			DrillLosses++;
		}
		if (DrillResultMessage.IsEmpty())
		{
			switch (SelectedDrill.VictoryType)
			{
			case EDrillVictoryType::HitShot:
				DrillResultMessage = TEXT("Drill Failed! You need to damage a bot.");
				break;
			case EDrillVictoryType::Location:
				DrillResultMessage = TEXT("Drill Failed! You need to reach the end location.");
				break;
			case EDrillVictoryType::MovementSpeed:
				DrillResultMessage = TEXT("Drill Failed! You need to reach at least ");
				DrillResultMessage.Append(FString::FromInt(SelectedDrill.DrillVictoryAmount)).Append("kph.");
				break;
			case EDrillVictoryType::FlagCaught:
				DrillResultMessage = TEXT("Drill Failed! You need to catch the flag in the air.");
				break;
			case EDrillVictoryType::NoFlagCarrier:
				DrillResultMessage = TEXT("Drill Failed! Enemy team has the flag.");
				break;
			case EDrillVictoryType::TotalKills:
				DrillResultMessage = TEXT("Drill Failed! You needed to kill ");
				DrillResultMessage.Append(FString::FromInt(SelectedDrill.DrillVictoryAmount)).Append(TEXT(" bots, but "));
				if (DrillKillCounter == 0)
				{
					DrillResultMessage.Append(TEXT("you didn't kill any!"));
				}
				else {
					DrillResultMessage.Append(TEXT("only killed ")).Append(FString::FromInt(DrillKillCounter)).Append(".");
				}
				break;
			case EDrillVictoryType::TotalMidairs:
				DrillResultMessage = TEXT("Drill Failed! You needed to hit ");
				DrillResultMessage.Append(FString::FromInt(SelectedDrill.DrillVictoryAmount)).Append(TEXT(" midair shots, but "));
				if (DrillMidairCounter == 0)
				{
					DrillResultMessage.Append(TEXT("you didn't hit any!"));
				}
				else {
					DrillResultMessage.Append(TEXT("only hit ")).Append(FString::FromInt(DrillMidairCounter)).Append(".");
				}
				break;
			}
		}

	}

	if (AMAPlayerController* PC = Cast<AMAPlayerController>(ParentController))
	{
		if (bIsDrillRunningAsTutorial)
		{
			if (bDrillWon)
			{
				DrillResultMessage = "Tutorial Step Completed!";
			}
			else {
				DrillResultMessage = "Tutorial Step Failed. Try Again?";
			}
			PC->ClientSay_Implementation(nullptr, DrillResultMessage, false);
		}
		else if (bIsDrillRunningAsWatcher)
		{
			DrillResultMessage = "Now you try!";
			PC->ClientSay_Implementation(nullptr, DrillResultMessage, false);
		}
		else {
			PC->ClientSay_Implementation(nullptr, DrillResultMessage, false);
			FString DrillResults = TEXT("Overall results: ");
			DrillResults.Append(FString::FromInt(DrillVictories)).Append(TEXT("/")).Append(FString::FromInt(DrillLosses + DrillVictories));
			PC->ClientSay_Implementation(nullptr, DrillResults, false);
		}
	}


	ParentController->GetWorldTimerManager().SetTimer(TimerHandle_DrillMessageClear, this, &UMAPracticeComponent::ClearDrillResultMessage, 5.0f, true, 5.0f);
}