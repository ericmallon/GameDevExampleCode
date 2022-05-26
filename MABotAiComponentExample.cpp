/*
* 
* Primary component for handling bot AI -- decision making, movement, aiming.
* General approach is to run DetermineCurrentTask every half second, which examines the game state and bot state to determine
* which action should be taken, who their target should be, and where they should move. DetermineCurrentTask uses a behavior weighting system to determine what action to perform.
*
* Bots are assigned positions relevant to gameplay on creation (Stay at home, chaser, offense, LO, route runner)
* and how they react is influenced by these positions. 
*
* Opon the bot component actually ticking, it will do its best to carry out the actions decided upon via DetermineCurrentTask
*
* Further enhancements would involve a (much) more intelligent movement system, primarily to handle movement around/near base geometry, and a team coordinator that allows for 
* better intra-bot communication for flag tossing and flag stand clearing. 
*
*/

#include "MidairCE.h"
#include "MABotAIComponent.h"
#include "Perception/PawnSensingComponent.h"
#include "Player/AIPlayerController.h"
#include "Player/MACharacter.h"
#include "Game/CTF/MACTFFlag.h"
#include "Game/CTF/MACTFFlagBase.h"
#include "Kismet/KismetMathLibrary.h"

// Sets default values for this component's properties
UMABotAIComponent::UMABotAIComponent()
{
	//turn off pawn sense stuff by default
	PrimaryComponentTick.bCanEverTick = true;
}

void UMABotAIComponent::EnableBotAI()
{
	//skip initialization if AI is already on
	if (bBotInitialized)
	{
		return;
	}
	PawnSensingComp = NewObject<UPawnSensingComponent>(this, UPawnSensingComponent::StaticClass());
	PrimaryComponentTick.bCanEverTick = true;
	PawnSensingComp->OnSeePawn.AddDynamic(this, &UMABotAIComponent::OnPawnSeen);
	PawnSensingComp->bOnlySensePlayers = false;
	PawnSensingComp->SetSensingUpdatesEnabled(true);
	PawnSensingComp->bSeePawns = true;
	PawnSensingComp->bHearNoises = false;
	PawnSensingComp->SightRadius = 60000.0f;
	PawnSensingComp->RegisterComponent();
	if (ParentCharacter != nullptr)
	{
		ParentCharacter->GetWorldTimerManager().SetTimer(TimerHandle_DetermineCurrentTask, this, &UMABotAIComponent::DetermineCurrentTask, 0.5f, true);
	}

	//initialize flag related game state
	for (TActorIterator<AMACTFFlagBase> ActorItr(GetWorld()); ActorItr; ++ActorItr)
	{
		AMACTFFlagBase *Stand = *ActorItr;
		uint8 BotTeamId = ParentCharacter->GetTeamId();
		uint8 StandTeamId = Stand->GetTeamId();
		bool bEnemyStand = BotTeamId != StandTeamId;
		if (bEnemyStand)
		{
			GameState.EnemyStandLocation = Stand->GetActorLocation();
		}
		else {
			GameState.FriendlyStandLocation = Stand->GetActorLocation();
		}
	}
	AccuracyLevel = BotConfig.AccuracyLevel;
	bBotInitialized = true;
}


// Called every frame
void UMABotAIComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	if (ParentCharacter == nullptr || ParentCharacter->GetController() == nullptr || bIsDead)
	{
		return;
	}
	//Guard against missing initialization upon bot creation
	if (!bBotInitialized)
	{
		AAIPlayerController* AIPC = Cast<AAIPlayerController>(ParentCharacter->GetController());
		AIPC->SetBotConfig(AIPC->BC); 
		return;
	}
	//can tick before we actually determine AI state, so just hard force this to always be correct, 
	//route runner bots do nothing but follow a recorded path
	if (BotConfig.BotType == EBotTypes::RouteRunner)
	{
		AIState.CurrentTask = EAIStates::RouteRunner;
	}
	//reset current target if no longer valid (ie, dead, switched teams, left server, etc)
	if (AIState.CurrentTarget != nullptr && (!IsValid(AIState.CurrentTarget)
		|| !FMath::IsNearlyZero(AIState.CurrentTarget->TimeOfDeath) || FMath::IsNearlyZero(AIState.CurrentTarget->GetHealth())))
	{
		AIState.CurrentTarget = nullptr;
	}

	//Each tick, we merely follow our current desired behavior, behavior definition is determined in DetermineCurrentTask less frequently.
	switch(AIState.CurrentTask)
	{
	case(EAIStates::ShootAtTarget):
	{
		ShootAtTarget();
		//We want to be moving around a bit randomly in addition to most of our states, to make the bot feel more natural and harder to hit
		MoveAround();
		break;
	}
	case(EAIStates::ChangeTarget):
	{
		ChangeTarget();
		MoveAround();
		break;
	}
	case(EAIStates::WaitForBetterShot):
	{
		WaitForBetterShot();
		MoveAround();
		break;
	}
	case(EAIStates::LookingForEnemy):
	{
		LookForEnemies();
		MoveAround();
		break;
	}
	case(EAIStates::MoveToTarget):
	{
		MoveToTarget();
		break;
	}
	case(EAIStates::RouteRunner):
	{
		RunRouteSimple();
		break;
	}
	}
}

//Ticks every half second (todo-emallon: make this configurable so we can reduce client load if they are running it locally),
//determining what actions/states the bot actor should be taking. General approach is to give all possible tasks a weighting, increasing in
//likelihood they take that action based on the situation. Weight added to various possible states is influenced by the bots assigned role.
//Highest weighted task option is chosen to be performed.
//todo-emallon try out choosing randomly-ish from all possible tasks, probably with an extra weight added to the 'winner'.
void UMABotAIComponent::DetermineCurrentTask()
{
	if (ParentCharacter == nullptr || bIsDead)
	{
		return;
	}
	TMap< EAIStates, float> TaskWeights;
	EAIStates LastTask = AIState.CurrentTask;
	//default to looking around if we have nothing else to do
	AIState.CurrentTask = EAIStates::LookingForEnemy;
	//we don't want to do the same thing for too long, so we track how long we have been doing our last task to bias against it
	float TimeSinceTaskChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfTaskStart;
	
	AAIPlayerController* AIPC = Cast<AAIPlayerController>(ParentCharacter->GetController());
	UMAPracticeComponent* PracticeComponent = AIPC->PracticeComponent;

	//if our goal in life is to just run a route, we ignore everything else.
	if (BotConfig.BotType == EBotTypes::RouteRunner)
	{
		AIState.CurrentTask = EAIStates::RouteRunner;
		RecentlySeenTargets.Reset();
		return;
	}

	//if we have decided to shoot but haven't yet, continue looking towards our shot. But never for more than a second
	if (AIState.bPendingWeaponFire && TimeSinceTaskChange < 1.0f)
	{
		RecentlySeenTargets.Reset();
		AIState.CurrentTask = EAIStates::ShootAtTarget;
		return;
	}

	//Route running bots need to figure out what route they are running prior to us running the determineMoveLoc code
	if (BotConfig.BotType == EBotTypes::Offense && AIState.RouteState == EAIRouteState::NoRouteSelected)
	{
		DetermineRouteToRun();
	}

	//Figure out where we should move to -- a target player, one of the flags, our route start.
	DetermineMoveLocation();

	//display a line pointer for each bot to their desired move location
	if (bBotDebugMode)
	{
		ClientDrawDebugLine(
			ParentCharacter->GetActorLocation(),
			AIState.DesiredMoveLocation,
			FColor(0, 255, 0),
			2.0f
		);
	}
	

	//handle our target being dead so we can reset it.
	if (AIState.CurrentTarget != nullptr && (!IsValid(AIState.CurrentTarget) || FMath::IsNearlyZero(AIState.CurrentTarget->GetHealth())))
	{
		AIState.CurrentTarget = nullptr;
	}

	//We need to ensure we are periodically looking around for new targets, and changing our movement directions.
	float TimeSinceLastCheckedForEnemies = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastLookForEnemy;
	float TimeSinceLastMovementChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastMovementTargetChange;

	float DistanceToMoveLocation = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AIState.DesiredMoveLocation);
	if (BotConfig.BotType == EBotTypes::Offense)
	{
		//If we are on O, try to move to our route start or if we are close enough, trigger the route follow to begin.
		if (AIState.RouteState == EAIRouteState::MovingToRouteStart)
		{
			//if we can't quite get to our route start we just teleport there. If they get stuck for a while, increase our teleport distance so they don't do stupid things.
			//we can improve this later when we have better movement code. todo-emallon
			//3s = 3 * 3 * 10 = 90
			//10s = 10 * 10 * 10 = 1000
			//20s = 20 * 20 * 10 = 4000
			//but cap it so we don't get super weird teleports

			if (AIState.MoveTargetType == EAIMoveTargetTypes::RouteStart && DistanceToMoveLocation < TimeSinceLastMovementChange * TimeSinceLastMovementChange * 10 && DistanceToMoveLocation < 5000)
			{
				StartRouteFollow();
			}
			else {
				TaskWeights.Add(EAIStates::MoveToTarget, 70.0f);
			}

		}
		//while running a route, we only care about changing tasks if we have overshot the flag.
		if (AIState.RouteState == EAIRouteState::RunningRoute)
		{
			//determine where we are, and where the grab happens so we can figure out if we are past it
			int priorMarkerNumber = FMath::Clamp(PracticeComponent->CurrentMarkerIndex - 1, 0, PracticeComponent->RouteTrailToRun.MarkerLocations.Num());
			int GrabMarker = AIState.CurrentRoute.GrabTime / PracticeComponent->PathRecordMarkerInterval / PracticeComponent->ModulusForLowPrecisionRecordMarkers;

			//if we are past our grab time and don't have the flag, we aren't going to be grabbing, so stop our route to clear
			//Or, if we are past the end of our route, abandon it.
			if ((priorMarkerNumber > GrabMarker && ParentCharacter->CarriedObject == nullptr)
				|| priorMarkerNumber == PracticeComponent->RouteTrailToRun.MarkerLocations.Num() - 2)
			{
				AIState.RouteState = EAIRouteState::AbandonedRoute;
				PracticeComponent->EndRoutePathPlayback();
			}
			else {
				TaskWeights.Add(EAIStates::RunningRoute, 170.0f);
			}
			//todo-emallon add code to abandon route if they are heavily damaged prior to attempting to grab the flag.
			//if damaged on route MORE than we expect we should be (due to disc jumps or whatever), 
			//if (FMath::IsNearlyEqual(ParentCharacter->GetHealth(), PracticeComponent->RouteTrailToRun.MarkerLocations[priorMarkerNumber].Health))

		}
		if (AIState.RouteState == EAIRouteState::RouteFinished)
		{
			if (AIState.MoveTargetType == EAIMoveTargetTypes::FriendlyStand && AIState.bIsHoldingFlag) {
				if (GameState.FlagState == EAIFlagStates::EnemyFlagTakenFriendlySafe)
				{
					//if we are trying to cap, that is always most important.
					TaskWeights.Add(EAIStates::MoveToTarget, 200.0f);
				}
				else {
					//otherwise stay close to the flag
					TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 15.0f, 150.0f));
				}
			}
			else {
				//If the route is over and we don't have flag, just respawn. 
				//todo-emallon, once we get a team coordiator to handle bot crosstalk better, they can clear if someone else is coming in.
				AIPC->Suicide();
				OnDied();
				return;
			}
		}
		if (AIState.RouteState == EAIRouteState::AbandonedRoute)
		{
			//if we have the flag, try to cap if home, or get close if it isn't.
			if (AIState.bIsHoldingFlag && (DistanceToMoveLocation > 3000 || GameState.bFriendlyFlagHome))
			{
				TaskWeights.Add(EAIStates::MoveToTarget, 200.0f);				
			}
			else {
				//if we abandoned our route, and don't have the flag, and haven't spawned in a while, suicide and start running routes again.
				if (GetWorld()->GetTimeSeconds() - TimeOfLastSpawn > 10 && !AIState.bIsHoldingFlag && GameState.bEnemyFlagHome)
				{
					AIPC->Suicide();
					OnDied();
					return;
				}
				else {
					//otherwise default to at least going somewhere.
					TaskWeights.Add(EAIStates::MoveToTarget, 20.0f);
				}
				
			}
		}
	}
	if (BotConfig.BotType == EBotTypes::Chase)
	{
		if (AIState.MoveTargetType == EAIMoveTargetTypes::FriendlyFlag && !GameState.bFriendlyFlagHome)
		{
			if (DistanceToMoveLocation < 10000 || AIState.CurrentTarget == nullptr)
			{
				//if we are close to a return, or we have no target, we care most about that
				TaskWeights.Add(EAIStates::MoveToTarget, 200.0f);
			}
			else {
				//otherwise, if flag isn't home, going towards it is generally quite important.
				TaskWeights.Add(EAIStates::MoveToTarget, 70.0f);
			}
			
		}
		else {
			//if we are too far from our stand, and our flag is home, respawn to get closer again.
			if (DistanceToMoveLocation > 20000 && GameState.bFriendlyFlagHome == true)
			{
				AIPC->Suicide();
				OnDied();
				return;
			}
			//always care at least a bit about the flag location, unless we are super close to ours already and dont need to return.
			if (DistanceToMoveLocation > 500)
			{
				TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 5.0f, 110.0f));
			}
			
		}
		
	}
	if (BotConfig.BotType == EBotTypes::LO)
	{
		//as LO, we are a bit more biased towards killing anything we see
		if (AIState.MoveTargetType != EAIMoveTargetTypes::EnemyStand || DistanceToMoveLocation > 400)
		{
			if (AIState.MoveTargetType == EAIMoveTargetTypes::FriendlyFlag && !GameState.bFriendlyFlagHeld && !GameState.bFriendlyFlagHome)
			{
				TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 10.0f, 400.0f));
			}
			else {
				TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 30.0f, 40.0f));
			}
			
		}
		else {
			TaskWeights.Add(EAIStates::LookingForEnemy, 10.0f);
		}
		
		
	}
	if (BotConfig.BotType == EBotTypes::StayAtHome)
	{
		//if enemy flag is in field, SaH generally wants to go pick it up, unless it is really far.
		if (AIState.MoveTargetType == EAIMoveTargetTypes::EnemyFlag && GameState.bEnemyFlagHeld == false && AIState.bIsHoldingFlag == false)
		{
			TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 50) / 100, 65.0f, 150.0f));
		}
		//if friendly flag is nearby for a return, also very important
		else if (AIState.MoveTargetType == EAIMoveTargetTypes::FriendlyFlag && GameState.bFriendlyFlagHome == false && GameState.bFriendlyFlagHeld == false && AIState.bIsHoldingFlag == false)
		{
			TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 20.0f, 100.0f));
		}
		else {
			TaskWeights.Add(EAIStates::MoveToTarget, FMath::Clamp((DistanceToMoveLocation - 500) / 100, 5.0f, 110.0f));
			TaskWeights.Add(EAIStates::LookingForEnemy, 6.0f);
		}
		
	}
	
	if (RecentlySeenTargets.Num() == 0 && AIState.CurrentTarget == nullptr)
	{
		//here, we have no good target
		//increase our desire to look for dudes by 5 every second
		float LookForEnemyTaskWeight = TimeSinceLastCheckedForEnemies * 5.0f;
		//if we have already been looking recently, we don't need to KEEP looking. Don't start looking if we haven't been doing something for long
		if ((LastTask != EAIStates::LookingForEnemy && TimeSinceTaskChange <= 2.0f) || (LastTask == EAIStates::LookingForEnemy && TimeSinceTaskChange > 2.0f))
		{
			LookForEnemyTaskWeight = 3.0f;
		}
		TaskWeights.Add(EAIStates::LookingForEnemy, LookForEnemyTaskWeight);
	} else {
		//we have at least one target, need to determine how badly we want to shoot at them, or if we should be waiting for a better shot
		float TargetHeightAboveGround = 9999999.0f;
		if (AIState.CurrentTarget != nullptr)
		{
			TargetHeightAboveGround = GetHeightAboveGround(AIState.CurrentTarget->GetActorLocation(), false);
		}

		float WaitForBetterShotWeight = 0.0f;
		if (AIState.CurrentTarget != nullptr)
		{
			// if close to ground AND falling
			if (TargetHeightAboveGround < 1000 && AIState.CurrentTarget->GetVelocity().Z < -200.0f)
			{
				WaitForBetterShotWeight += 9.0f;
			}
		}
		float ChangeTargetWeight = 0.0f;
		if (RecentlySeenTargets.Num() > 0)
		{
			AMACharacter* MostDesirableTarget = AIState.CurrentTarget;
			float HighestFocusScore = 0.0f;
			//first prune any targets that might have died/left/whatever.
			TMap<AMACharacter*, float> ValidRecentlySeenTargets;
			for (auto Element : RecentlySeenTargets)
			{
				float WorldTimeLastSeen = Element.Value; //was working on giving memory to bots again
				if (Element.Key != nullptr && Element.Key && IsValid(Element.Key)  && Element.Key->IsValidLowLevel() && Element.Key->GetDebugName(Element.Key).Contains("BP_LightCharacter")
					&& ParentCharacter->GetWorld()->GetTimeSeconds() - WorldTimeLastSeen < 5.0f)
				{
					ValidRecentlySeenTargets.Add(Element);
				}
			}
			RecentlySeenTargets = ValidRecentlySeenTargets;
			for (auto Element : RecentlySeenTargets)
			{
				//fetch how desirable this particular target is, so we can find who best to shoot.
				float FocusScoreForTarget = GetTargetFocusScore(Element.Key);
				if (FocusScoreForTarget > HighestFocusScore)
				{
					HighestFocusScore = FocusScoreForTarget;
					MostDesirableTarget = Element.Key;
				}
			}
			
			if (MostDesirableTarget == AIState.CurrentTarget)
			{
				bool bCanSeeTarget = AimAtTarget(false);
				if (bCanSeeTarget)
				{
					TaskWeights.Add(EAIStates::ShootAtTarget, HighestFocusScore);
				}
			}
			else {
				TaskWeights.Add(EAIStates::ChangeTarget, HighestFocusScore);
				AIState.CurrentTarget = MostDesirableTarget;
			}
		}
		else {
			ParentCharacter->SetTrigger(0, false);
		}
	}
	//if we have the flag and the flag is home, nothing else matters over getting there.
	if (AIState.bIsHoldingFlag && GameState.bFriendlyFlagHome)
	{
		TaskWeights.Add(EAIStates::MoveToTarget, 9001.0f);
	}

	float MaxTaskWeight = 0.0f;
	for (auto Element : TaskWeights)
	{
		if (Element.Value > MaxTaskWeight)
		{
			MaxTaskWeight = Element.Value;
			AIState.CurrentTask = Element.Key;
		}
	}

	//if we are moving to the stand but can't actually DO anything there, switch to look for targets/wander so we prevent the spinning in place issues
	//if at the enemy stand and the flag isn't home, look for things to shoot.
	//if at the friendly flag and we aren't holding the flag and the flag isn't home (aka we are capping), look for enemies to shoot.
	if(AIState.CurrentTask == EAIStates::MoveToTarget && DistanceToMoveLocation < 300 
		&& ((AIState.MoveTargetType == EAIMoveTargetTypes::EnemyStand && GameState.bEnemyFlagHome == false)
			|| (AIState.MoveTargetType == EAIMoveTargetTypes::FriendlyStand && (AIState.bIsHoldingFlag == false || GameState.bFriendlyFlagHome == false))))
	{
		AIState.CurrentTask = EAIStates::LookingForEnemy;
	}


	FString AISTateString = "Undefined";
	switch (AIState.CurrentTask)
	{
	case(EAIStates::ChangeTarget):
		AISTateString = "Change Target";
		break;
	case(EAIStates::LookingForEnemy):
		AISTateString = "Looking for Enemy";
		break;
	case(EAIStates::MoveToTarget):
		AISTateString = "Move To Target";
		break;
	case(EAIStates::RouteRunner):
		AISTateString = "Route RUnner";
		break;
	case(EAIStates::RunningRoute):
		AISTateString = "Running ROute";
		break;
	case(EAIStates::ShootAtTarget):
		AISTateString = "Shoot at target";
		break;
	case(EAIStates::WaitForBetterShot):
		AISTateString = "Wait for better shot";
		break;
	}
	AISTateString = "Task: " + AISTateString;
	//GEngine->AddOnScreenDebugMessage(73, 130.1f, FColor::Black, AISTateString);
	//GEngine->AddOnScreenDebugMessage(75, 130.1f, FColor::Red, FString::Printf(TEXT("task time: %f"), TimeSinceTaskChange));

	if (AIState.CurrentTask != LastTask)
	{
		TimeOfTaskStart = ParentCharacter->GetWorld()->GetTimeSeconds();
		AIState.IsTaskInitialized = false;
	}
	//todo-emallon hack that removes the bots memory. Right now we aren't properly pruning recently seen targets when characters die
	//so we crash when checking focus scores for the already dead targets sometimes.
	//need to probably change this to a map of TWeakObjectPtr instead.
	RecentlySeenTargets.Reset();
}

void UMABotAIComponent::DetermineRouteToRun()
{
	//if we haven't added any routes to our capper, we can't choose a route, now can we?
	if (BotConfig.RouteTrailNames.Num() == 0) 
	{
		return;
	}
	if (AAIPlayerController* AIPC = Cast<AAIPlayerController>(ParentCharacter->GetController()))
	{
		int BotRouteToRun = FMath::RandRange(0, BotConfig.RouteTrailNames.Num() - 1);
		FString BotRoute = BotConfig.RouteTrailNames[BotRouteToRun];
		UMAPracticeComponent* PracticeComponent = AIPC->PracticeComponent;

		int TeamID = 0;
		if (AMAPlayerState* PS = Cast<AMAPlayerState>(ParentCharacter->GetController()->PlayerState))
		{
			TeamID = PS->GetTeamId();
		}
		AIState.CurrentRoute = PracticeComponent->GetRouteTrailByName(BotRoute, TeamID); //todo-emallon use a RouteTrailLite here so we don't pass all marker locations/input around
		if (AIState.CurrentRoute.MarkerLocations.Num() > 0)
		{
			AIState.RouteStartLocation = AIState.CurrentRoute.MarkerLocations[0].Location;
		}
		AIState.RouteState = EAIRouteState::MovingToRouteStart;
	}
}

void UMABotAIComponent::DetermineMoveLocation()
{
	if (ParentCharacter == nullptr)
	{
		return;
	}
	float TargetDistance = DistanceToTarget(AIState.CurrentTarget);
	FVector OriginalMoveLocation = AIState.DesiredMoveLocation;
	EAIMoveTargetTypes OriginalMoveLocationType = AIState.MoveTargetType;
	AIState.DesiredMoveLocation = FVector::ZeroVector;
	//if we need to start a route, then we just go ASAP to route start.
	if (BotConfig.BotType == EBotTypes::Offense && AIState.RouteState == EAIRouteState::MovingToRouteStart)
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::RouteStart;
		AIState.DesiredMoveLocation = AIState.RouteStartLocation;
		return;
	}
	//first, get the latest game state.
	for (TActorIterator<AMACTFFlag> ActorItr(GetWorld()); ActorItr; ++ActorItr)
	{
		AMACTFFlag *Flag = *ActorItr;
		uint8 BotTeamId = ParentCharacter->GetTeamId();
		uint8 FlagTeamId = Flag->GetTeamId();
		bool bEnemyFlag = BotTeamId != FlagTeamId;
		if (bEnemyFlag)
		{
			GameState.EnemyFlagLocation = Flag->GetActorLocation();
			GameState.bEnemyFlagHome = Flag->IsHome();
			GameState.bEnemyFlagHeld = Flag->StateName == CarriedObjectState::Held;
			AIState.DistanceToEnemyFlag = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), Flag->GetActorLocation());
		}
		else {
			GameState.FriendlyFlagLocation = Flag->GetActorLocation();
			GameState.bFriendlyFlagHome = Flag->IsHome();
			GameState.bFriendlyFlagHeld = Flag->StateName == CarriedObjectState::Held;
			AIState.DistanceToFriendlyFlag = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), Flag->GetActorLocation());
		}
	}
	if (GameState.bEnemyFlagHome && GameState.bFriendlyFlagHome)
	{
		GameState.FlagState = EAIFlagStates::BothFlagsHome;
	}else if (!GameState.bEnemyFlagHome && GameState.bFriendlyFlagHome)
	{
		GameState.FlagState = EAIFlagStates::EnemyFlagTakenFriendlySafe;
	}else if (GameState.bEnemyFlagHome && !GameState.bFriendlyFlagHome)
	{
		GameState.FlagState = EAIFlagStates::FriendlyTakenEnemyHome;
	}else if (!GameState.bEnemyFlagHome && !GameState.bFriendlyFlagHome)
	{
		GameState.FlagState = EAIFlagStates::Standoff;
	}
	
	AIState.bIsHoldingFlag = ParentCharacter->CarriedObject != nullptr;

	//If we have the flag we always try to cap. 
	if (AIState.bIsHoldingFlag)
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyStand;
		AIState.DesiredMoveLocation = GameState.FriendlyStandLocation;
	}
	//If chase, we always care about our flag unless we are holding.
	if (!AIState.bIsHoldingFlag && BotConfig.BotType == EBotTypes::Chase)
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyFlag;
		AIState.DesiredMoveLocation = GameState.FriendlyFlagLocation;
	}
	//if we are on O, we care about returns in standoffs and otherwise the enemy flag.
	if (!AIState.bIsHoldingFlag && (BotConfig.BotType == EBotTypes::Offense || BotConfig.BotType == EBotTypes::LO))
	{
		//if enemy flag is dropped and close, we go for that
		if (GameState.bEnemyFlagHome == false && GameState.bEnemyFlagHeld == false && AIState.DistanceToEnemyFlag < 5000) 
		{
			AIState.MoveTargetType = EAIMoveTargetTypes::EnemyFlag;
			AIState.DesiredMoveLocation = GameState.EnemyFlagLocation;
		}
		//if friendly flag is dropped and close, we prioritize that next
		else if (GameState.bFriendlyFlagHeld == false && GameState.bFriendlyFlagHome == false && AIState.DistanceToFriendlyFlag < 5000) 
		{
			AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyFlag;
			AIState.DesiredMoveLocation = GameState.FriendlyFlagLocation;
		}else if (GameState.FlagState == EAIFlagStates::Standoff)
		{
			AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyFlag;
			AIState.DesiredMoveLocation = GameState.FriendlyFlagLocation;
		} else {
			if (BotConfig.BotType == EBotTypes::Offense)
			{
				AIState.MoveTargetType = EAIMoveTargetTypes::EnemyFlag;
				AIState.DesiredMoveLocation = GameState.EnemyFlagLocation;
			}
			else if (BotConfig.BotType == EBotTypes::LO)
			{
				if (GameState.FlagState == EAIFlagStates::EnemyFlagTakenFriendlySafe)
				{
					AIState.MoveTargetType = EAIMoveTargetTypes::EnemyFlag;
					AIState.DesiredMoveLocation = GameState.EnemyFlagLocation;
				}
				else {
					AIState.MoveTargetType = EAIMoveTargetTypes::EnemyStand;
					AIState.DesiredMoveLocation = GameState.EnemyStandLocation;
				}
				
			}
			
		}
		
	}
	//Stay at home cares about friendly flag before standoffs, and enemy during standoffs.
	if (BotConfig.BotType == EBotTypes::StayAtHome)
	{
		//in general, SaH goes to their own stand
		AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyStand;
		AIState.DesiredMoveLocation = GameState.FriendlyStandLocation;

		//if you are in a standoff and the flag is close to you, try to pick it up
		if (GameState.FlagState == EAIFlagStates::Standoff 
			|| (GameState.FlagState == EAIFlagStates::EnemyFlagTakenFriendlySafe && AIState.DistanceToEnemyFlag < 10000 && GameState.bEnemyFlagHeld == false))
		{
			AIState.MoveTargetType = EAIMoveTargetTypes::EnemyFlag;
			AIState.DesiredMoveLocation = GameState.EnemyFlagLocation;
		}
		else {
			//if friendly flag has been taken, and is close and we don't have their flag, chase.
			if (GameState.FlagState == EAIFlagStates::FriendlyTakenEnemyHome && AIState.DistanceToFriendlyFlag < 10000)
			{
				AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyFlag;
				AIState.DesiredMoveLocation = GameState.FriendlyFlagLocation;
			}			
		}
	}
	float DistanceToMoveLocation = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AIState.DesiredMoveLocation);
	//if we are relatively close to where we want to be and have a target, go for our target.
	if (AIState.CurrentTarget != nullptr && TargetDistance < 20000 && DistanceToMoveLocation < 10000)
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::EnemyTarget;
		AIState.DesiredMoveLocation = AIState.CurrentTarget->GetActorLocation();
	}

	//distance to flag where it being on the ground overrides everything else, differs per position
	float FriendlyFlagOverrideDistance = 5000.0f;
	float EnemyFlagOverrideDistance = 5000.0f;

	if (BotConfig.BotType == EBotTypes::StayAtHome)
	{
		EnemyFlagOverrideDistance = 15000;
		FriendlyFlagOverrideDistance = 10000;
	}
	if (BotConfig.BotType == EBotTypes::Chase)
	{
		FriendlyFlagOverrideDistance = 15000;
	}
	//if the flag is in the field, we can care about that most, usually.
	if (AIState.DistanceToFriendlyFlag < FriendlyFlagOverrideDistance && !GameState.bFriendlyFlagHeld 
		&& (GameState.FlagState == EAIFlagStates::FriendlyTakenEnemyHome || GameState.FlagState == EAIFlagStates::Standoff))
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyFlag;
		AIState.DesiredMoveLocation = GameState.FriendlyFlagLocation;
	}
	if (AIState.DistanceToEnemyFlag < EnemyFlagOverrideDistance && !GameState.bEnemyFlagHeld 
		&& (GameState.FlagState == EAIFlagStates::EnemyFlagTakenFriendlySafe || GameState.FlagState == EAIFlagStates::Standoff))
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::EnemyFlag;
		AIState.DesiredMoveLocation = GameState.EnemyFlagLocation;
	}

	//If we have the flag and can cap, we always try to cap. 
	if (AIState.bIsHoldingFlag && GameState.bFriendlyFlagHome)
	{
		AIState.MoveTargetType = EAIMoveTargetTypes::FriendlyStand;
		AIState.DesiredMoveLocation = GameState.FriendlyStandLocation;
	}
	//not fully accurate yet, doesnt track changing enemy targets.
	if (OriginalMoveLocationType != AIState.MoveTargetType)
	{
		TimeOfLastMovementTargetChange = GetWorld()->GetTimeSeconds();

		//start of prototype multi-waypoint moves to allow for planning of more complex movements around geometry. barely started.
		/* FVector StartLocation = ParentCharacter->GetActorLocation();
		FVector EndLocation = AIState.DesiredMoveLocation;           // Raytrace end point.

		FCollisionQueryParams CollisionParams = FCollisionObjectQueryParams::AllStaticObjects;
		CollisionParams.AddIgnoredActor(ParentCharacter);

		// Raytrace for overlapping actors.
		FHitResult HitResult;
		if (GetWorld())
		{
			GetWorld()->LineTraceSingleByObjectType(
				OUT HitResult,
				StartLocation,
				EndLocation,
				FCollisionObjectQueryParams(ECollisionChannel::ECC_WorldStatic),
				CollisionParams
			);
			FColor LineColor;

			if (HitResult.GetActor()) LineColor = FColor::Red;
			else LineColor = FColor::Green;

			DrawDebugLine(
				ParentCharacter->GetWorld(),
				StartLocation,
				EndLocation,
				LineColor,
				true,
				10.f,
				ESceneDepthPriorityGroup::SDPG_World,
				10.f
			);

			//we hit something in between us and flag, try going up first.
			if (HitResult.GetActor())
			{
				FVector EndLocationWaypoint = AIState.DesiredMoveLocation;
				EndLocationWaypoint.Z += 600.0f; //200 =~ height of flag

				GetWorld()->LineTraceSingleByObjectType(
					OUT HitResult,
					StartLocation,
					EndLocationWaypoint,
					FCollisionObjectQueryParams(ECollisionChannel::ECC_WorldStatic),
					CollisionParams
				);

				if (HitResult.GetActor()) LineColor = FColor::Red;
				else LineColor = FColor::Green;

				DrawDebugLine(
					ParentCharacter->GetWorld(),
					StartLocation,
					EndLocationWaypoint,
					LineColor,
					true,
					10.f,
					ESceneDepthPriorityGroup::SDPG_World,
					10.f
				);

				GetWorld()->LineTraceSingleByObjectType(
					OUT HitResult,
					EndLocationWaypoint,
					EndLocation,
					FCollisionObjectQueryParams(ECollisionChannel::ECC_WorldStatic),
					CollisionParams
				);


				if (HitResult.GetActor()) LineColor = FColor::Red;
				else LineColor = FColor::Green;

				DrawDebugLine(
					ParentCharacter->GetWorld(),
					EndLocationWaypoint,
					EndLocation,
					LineColor,
					true,
					10.f,
					ESceneDepthPriorityGroup::SDPG_World,
					10.f
				);
			}
		} */
		

	}

}

void UMABotAIComponent::ShootAtTarget()
{
	SelectBestWeapon();
	AimAtTarget(true);
}

void UMABotAIComponent::WaitForBetterShot()
{
	AimAtTarget(false);
	SelectBestWeapon();
	ParentCharacter->SetTrigger(0, false);
}

void UMABotAIComponent::ChangeTarget()
{
	AimAtTarget(false);
	ParentCharacter->SetTrigger(0, false);
}

void UMABotAIComponent::MoveToTarget()
{
	if (ParentCharacter == nullptr || ParentCharacter->GetController() == nullptr)
	{
		return;
	}
	float HeightAboveGround = GetHeightAboveGround(ParentCharacter->GetActorLocation(), false);
	float DistanceToDesiredLocation = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AIState.DesiredMoveLocation);
	//first, if we are far from our desired location (enemy player or our flag) we move towards them.

	FVector VectorToTarget = AIState.DesiredMoveLocation - ParentCharacter->GetActorLocation();
	FRotator RotatorToLookAtMoveLocation = UKismetMathLibrary::MakeRotFromXZ(VectorToTarget.GetSafeNormal(), ParentCharacter->GetActorUpVector());

	//if we just shot at something, we want to look at what we shot at, not at our move target, and then look back over time.
	//otherwise we get really jerky orientations from the bots.
	float TimeSinceLastShot = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastShot;
	//0s since last shot = look at shot, full skew 3-0=3/3=1
	//3s or greater - 0 skew 3-3=0/3=0, 3-1.5=1.5/3=.5
	float SkewFactor = (3.0f - FMath::Min(TimeSinceLastShot, 3.0f)) / 3.0f;
	RotatorToLookAtMoveLocation.Pitch += RandomPitchSkew * SkewFactor;
	RotatorToLookAtMoveLocation.Yaw += RandomYawSkew * SkewFactor;

	ParentCharacter->GetController()->SetControlRotation(RotatorToLookAtMoveLocation);
	FRotator ActorRot = RotatorToLookAtMoveLocation;
	ActorRot.Roll = 0.0f;
	ActorRot.Pitch = 0.0f;
	ParentCharacter->SetActorRotation(ActorRot);

	ParentCharacter->MoveForward(1.0f);

	//we don't want to skii if we are sliding backwards from our target, since we won't gain mommentum going the correct direction
	float DistanceToTargetPlusVelocity = DistanceToDesiredLocation + DistanceBetweenTargets(ParentCharacter->GetActorLocation() + ParentCharacter->GetVelocity(), AIState.DesiredMoveLocation);
	if (HeightAboveGround < 100 && DistanceToDesiredLocation > 1000 && DistanceToTargetPlusVelocity > DistanceToDesiredLocation)
	{
		ParentCharacter->Skate();
	}
	else {
		ParentCharacter->StopSkating();
	}
	float HeightAboveTargetLoc = HeightAbove(AIState.DesiredMoveLocation);
	//TODO improve amount of jets needed to go X height formula
	//1000 below, -1000. Z velocity goes to like 3-4k when skiing up fast. If we are close, and already have velocity, we stop jetting. 
	bool bWasPreviouslyJetting = bIsJetting;
	float TimeOfSinceJetChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastJetChange;
	float CharEnergy = ParentCharacter->GetEnergy();
	bool HeightAboveTargetCheck = HeightAboveTargetLoc < 0;
	//we want to give jet energy some time to recharge if it is low, before trying to jet
	bool EnergyRechargeCheck = (bWasPreviouslyJetting || TimeOfSinceJetChange > 2.0f || CharEnergy > 100);
	float VelocityZ = ParentCharacter->GetVelocity().Z;
	//controls how far above a target we overshoot so we don't accidentally not get all the way up.
	float OvershootFudgeFactor = 300.0f;
	//stop jetting early so we don't go WAY above it
	bool OvershootCheck = !(VelocityZ / 2 + HeightAboveTargetLoc > OvershootFudgeFactor && TimeOfSinceJetChange > 1.0f);
	//bots are bad with energy for now, so blatently cheat //todo-emallon REMOVE ME
	if (CharEnergy < 50.5f)
	{
		ParentCharacter->GetVitals()->SetEnergy(100.0f);
	}
	if (HeightAboveTargetCheck && EnergyRechargeCheck && OvershootCheck && CharEnergy > 0.01f)
	{
		bIsJetting = true;
		ParentCharacter->Jump();
		ParentCharacter->Jet();
	}
	else {
		bIsJetting = false;
		ParentCharacter->StopJumping();
		ParentCharacter->StopJetting();
	}
	if (bWasPreviouslyJetting != bIsJetting)
	{
		TimeOfLastJetChange = ParentCharacter->GetWorld()->GetTimeSeconds();
	}

}
//most of the time when bots are doing something (looking for enemies, shooting at someone, defending the flag, etc) we want them to be doing some minor movements
//to make them look more natural.
void UMABotAIComponent::MoveAround()
{
	if (ParentCharacter == nullptr || ParentCharacter->GetController() == nullptr || BotConfig.BotType == EBotTypes::StationaryDefense)
	{
		return;
	}
	float HeightAboveGround = GetHeightAboveGround(ParentCharacter->GetActorLocation(), false);
	float DistanceToDesiredLocation = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AIState.DesiredMoveLocation);
	//if we are already close to the target location, we move around randomly.
	float TimeSinceLastMovementChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastMovementChange;
	if (TimeSinceLastMovementChange > 1.0f)
	{
		if (FMath::RandRange(0.0f, 3.0f) + TimeSinceLastMovementChange > 3.0f)
		{
			//If we are where we want to be, and no enemy is close, we want to just chill and not move too randomly most of the time.
			if (AIState.CurrentTask == EAIStates::LookingForEnemy || ((AIState.CurrentTask == EAIStates::ShootAtTarget && DistanceToTarget(AIState.CurrentTarget) > 5000)
				&& FMath::RandRange(0, 3) > 1))
			{
				ActiveMovementType = EPlayerRecordableInputTypes::StopSkii;
			}
			else {
				int RandomMovementType = FMath::RandRange(0, 3);
				switch (RandomMovementType)
				{
				case 0:
					ActiveMovementType = EPlayerRecordableInputTypes::Forward;
					break;
				case 1:
					ActiveMovementType = EPlayerRecordableInputTypes::Backwards;
					break;
				case 2:
					ActiveMovementType = EPlayerRecordableInputTypes::Left;
					break;
				case 3:
					ActiveMovementType = EPlayerRecordableInputTypes::Right;
					break;
				}
			}
				
			TimeOfLastMovementChange = ParentCharacter->GetWorld()->GetTimeSeconds();
		}
	}
	float TimeSinceLastJetChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastJetChange;
	if (TimeSinceLastJetChange > 1.0f && (ParentCharacter->GetEnergy() > 40 || ParentCharacter->GetEnergy() < 5))
	{
		if (FMath::RandRange(0.0f, 3.0f) + TimeSinceLastJetChange > 3.0f)
		{
			bIsJetting = !bIsJetting;
			TimeOfLastJetChange = ParentCharacter->GetWorld()->GetTimeSeconds();
		}
	}
	//now that we figured out what we SHOULD do, we can implement it.
	//first, stop skiing, we are already close
	ParentCharacter->StopSkating();
	//then set jet status
	if (bIsJetting)
	{
		ParentCharacter->Jump();
		ParentCharacter->Jet();
	}
	else {
		ParentCharacter->StopJumping();
		ParentCharacter->StopJetting();
	}

	//and finally set where we are moving to. 
	switch (ActiveMovementType)
	{
	case(EPlayerRecordableInputTypes::Forward):
		ParentCharacter->MoveForward(1.0f);
		break;
	case(EPlayerRecordableInputTypes::Backwards):
		ParentCharacter->MoveForward(-1.0f);
		break;
	case(EPlayerRecordableInputTypes::Left):
		ParentCharacter->MoveRight(-1.0f);
		break;
	case(EPlayerRecordableInputTypes::Right):
		ParentCharacter->MoveRight(1.0f);
		break;
	}
}

//standard bot route running
void UMABotAIComponent::StartRouteFollow()
{
	if (AIState.IsTaskInitialized || AIState.CurrentRoute.MarkerLocations.Num() < 1)
	{
		return;
	}
	if (AAIPlayerController* AIPC = Cast<AAIPlayerController>(ParentCharacter->GetController()))
	{

		UMAPracticeComponent* PracticeComponent = AIPC->PracticeComponent;

		//set up options for how we want to run the route (these mostly matter for practice mode, and will always be like this for a real bot running AI)
		PracticeComponent->SelectedRouteTrail = AIState.CurrentRoute;
		PracticeComponent->RouteTrailMarkerIndex = 0;
		PracticeComponent->bResumePathAfterDamage = false;
		PracticeComponent->bStayAliveAfterRouteEnd = true;
		PracticeComponent->bRestoreHealthOnTeleport = false;
		//start the auto-follow of the selected route.
		PracticeComponent->MovePawnOnRoutePath();
		AIState.IsTaskInitialized = true;
		AIState.RouteState = EAIRouteState::RunningRoute;
		AIState.CurrentTask = EAIStates::RunningRoute;
	}
}

//Run a route in complete AFK mode, including spawning mid-route, will never exit early
void UMABotAIComponent::RunRouteSimple()
{
	if (AIState.IsTaskInitialized || BotConfig.RouteTrailNames.Num() < 1)
	{
		return;
	}
	if (AAIPlayerController* AIPC = Cast<AAIPlayerController>(ParentCharacter->GetController()))
	{
		int BotRouteToRun = FMath::RandRange(0, BotConfig.RouteTrailNames.Num() - 1);
		FString BotRoute = BotConfig.RouteTrailNames[BotRouteToRun];
		UMAPracticeComponent* PracticeComponent = AIPC->PracticeComponent;

		int TeamID = 0;
		if (AMAPlayerState* PS = Cast<AMAPlayerState>(ParentCharacter->GetController()->PlayerState))
		{
			TeamID = PS->GetTeamId();
		}

		FMARouteTrail RouteTrail = PracticeComponent->GetRouteTrailByName(BotRoute, TeamID);
		int MarkerIndexToSpawnBotAt = 0;
		if (BotConfig.BotSpawnType == EDrillBotSpawnType::SecondsBeforeGrab)
		{
			if (RouteTrail.GrabTime >= BotConfig.SpawnDelay)
			{
				float TimeAtWhichToSpawnBot = RouteTrail.GrabTime - BotConfig.SpawnDelay;
				MarkerIndexToSpawnBotAt = TimeAtWhichToSpawnBot / PracticeComponent->PathRecordMarkerInterval / (float)PracticeComponent->ModulusForPathRecordMarkers;
				//add some randomness to when they spawn
				MarkerIndexToSpawnBotAt -= FMath::RandRange(0, 8);
			}
			else {
				//todo-emallon handle delayed route starts for routes that we can't spawn immediately
			}
		}
		if (BotConfig.BotSpawnType == EDrillBotSpawnType::SecondsIntoRoute)
		{
			float TimeAtWhichToSpawnBot = BotConfig.SpawnDelay;
			MarkerIndexToSpawnBotAt = TimeAtWhichToSpawnBot / PracticeComponent->PathRecordMarkerInterval / (float)PracticeComponent->ModulusForPathRecordMarkers;
			MarkerIndexToSpawnBotAt -= FMath::RandRange(0, 8);
		}
		//make sure we have a valid marker index after we added a bit of randomness to it
		MarkerIndexToSpawnBotAt = FMath::Clamp(MarkerIndexToSpawnBotAt, 0, RouteTrail.MarkerLocations.Num() / PracticeComponent->ModulusForPathRecordMarkers);

		if (BotConfig.BotType == EBotTypes::RouteRunner)
		{
			PracticeComponent->SelectedRouteTrail = RouteTrail;
			PracticeComponent->RouteTrailMarkerIndex = MarkerIndexToSpawnBotAt > 0 ? MarkerIndexToSpawnBotAt : 0;
			PracticeComponent->bResumePathAfterDamage = !BotConfig.bBotAlwaysFollowPath;
			PracticeComponent->bRestoreHealthOnTeleport = !BotConfig.bBotTakesDamage;
			PracticeComponent->MovePawnOnRoutePath();
		}
		AIState.IsTaskInitialized = true;
	}
}

void UMABotAIComponent::LookForEnemies()
{
	TimeOfLastLookForEnemy = ParentCharacter->GetWorld()->GetTimeSeconds();
	ParentCharacter->SetTrigger(0, false);

	FRotator ControlRotation = ParentCharacter->GetController()->GetControlRotation();
	ControlRotation.Yaw += FMath::RandRange(0.0f, 5.0f); //todo-emallon LERP based on bot tick frequency and a delta T. this does look fine as is in all normal circumstances, but will fail if we are ticking much faster/slower
	ParentCharacter->GetController()->SetControlRotation(ControlRotation);
	FRotator ActorRot = ControlRotation;
	ActorRot.Roll = 0.0f;
	ActorRot.Pitch = 0.0f;
	ParentCharacter->SetActorRotation(ActorRot);
}
//based on the health/location/velocity of our target, choose what to shoot them with
//doesn't use nade yet, just disc + chain.
void UMABotAIComponent::SelectBestWeapon()
{
	//don't swap weapons if we just did it less than 2s ago
	if (AIState.CurrentTarget == nullptr || ParentCharacter == nullptr ||
		!FMath::IsNearlyZero(AIState.CurrentTarget->TimeOfDeath) ||(ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastWeaponChange) < 2.0f)
	{
		return;
	}
	float DiscWeight = 1.0f;
	float ChaingunWeight = 0.0f;
	float NadeWeight = 0.0f;
	if (AIState.CurrentTarget->GetHealth() < 50)
	{
		ChaingunWeight += 30.0f;
		DiscWeight += 5.0f;
	}
	//generally ground pound with disc, shoot flying targets with chain.
	float TargetHeightAboveGround = GetHeightAboveGround(AIState.CurrentTarget->GetActorLocation(), false);
	if (TargetHeightAboveGround < 600)
	{
		DiscWeight += 30.0f;
	}
	else {
		ChaingunWeight += 10.0f;
	}
	//chain better against faster targets
	if (GetTargetVelocity(AIState.CurrentTarget) > 160.0f)
	{
		ChaingunWeight += 15.0f;
	}
	//chain much than disc better against further targets
	float TargetDistance = DistanceToTarget(AIState.CurrentTarget);
	if (TargetDistance > 10000)
	{
		ChaingunWeight += 20.0f;
	}
	else if (TargetDistance < 3000)
	{
		DiscWeight += 20.0f;
	}
	// see if they are coming directly towards or away from us. If the angle is small, disc is more likely to be used, easier shot
	float TargetDistancePlusVelocity = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AIState.CurrentTarget->GetActorLocation() + GetTargetVelocity(AIState.CurrentTarget));
	if (FMath::Abs(TargetDistancePlusVelocity - TargetDistance) > 0.8 * AIState.CurrentTarget->GetVelocity().Size())
	{
		DiscWeight += 15.0f;
	}
	//...but if the config says to not use the weapon, don't.
	if (BotConfig.bNoChaingun)
	{
		ChaingunWeight = -100.0f;
	}
	if (BotConfig.bNoDisc)
	{
		DiscWeight = -100.0f;
	}
	FString WeaponClassName = ParentCharacter->Weapon->GetName();
	
	if (WeaponClassName.Contains("Chaingun"))
	{
		//discourage bad bots from chaining a lot
		float TimeSinceLastWeaponChange = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastWeaponChange;
		if (BotConfig.AccuracyLevel == EBotAccuracyLevels::Horrible && TimeSinceLastWeaponChange > 2.0f)
		{
			ChaingunWeight -= 50.0f;
		}else if (BotConfig.AccuracyLevel == EBotAccuracyLevels::Decent && TimeSinceLastWeaponChange > 3.0f)
		{
			ChaingunWeight -= 20.0f;
		}
	}

	if (DiscWeight > ChaingunWeight)
	{
		if (WeaponClassName.Contains("Chaingun"))
		{
			TimeOfLastWeaponChange = ParentCharacter->GetWorld()->GetTimeSeconds();
		}
		ParentCharacter->SwitchToWeaponAtIndex(0);
	}
	else {
		if (WeaponClassName.Contains("RingLauncher"))
		{
			TimeOfLastWeaponChange = ParentCharacter->GetWorld()->GetTimeSeconds();
		}
		ParentCharacter->SwitchToWeaponAtIndex(2);
	}
}

//determine how good a candidate the passed in target is to shoot at.
float UMABotAIComponent::GetTargetFocusScore(AMACharacter* Target)
{
	if (!Target || Target == nullptr || !Target->IsValidLowLevel() || !IsValid(Target) || Target->GetMesh1P() == nullptr)
	{
		return 0.0f;
	}
	float TargetFocusScore = 0.0f;
	//we like to keep shooting what we are already shooting
	if (Target == AIState.CurrentTarget)
	{
		TargetFocusScore += 30.0f;
	}
	//low HP target -- how low their HP is, from 0 - 20
	TargetFocusScore += (200.0f - Target->GetHealth()) / 10;
	//slower targets -- kph 0 - 40 (can be negative too if they are faster than 200)
	TargetFocusScore += (200.0f - GetTargetVelocity(Target)) / 5;
	//close to ground
	float TargetHeightAboveGround = GetHeightAboveGround(Target->GetActorLocation(), false);
	if (TargetHeightAboveGround < 200)
	{
		TargetFocusScore += 30.0f;
	}
	//close targets
	TargetFocusScore += FMath::Clamp((10000.0f - DistanceToTarget(Target)) / 100.0f, -100.0f, 40.0f); //TODO-EMALLON figure out weighting for this.

	//we really like shooting the carrier
	if (Target->CarriedObject != nullptr)
	{
		TargetFocusScore += 50.0f;
	}

	//we want to make some of these negative -- a really far target is NOT desirable at all, even if other items are good.
	//could probably break out of the function early in those cases too, to save perf
	return TargetFocusScore;
}
//convert from engine units to KPH
float UMABotAIComponent::GetTargetVelocity(AMACharacter* Target)
{
	if (!Target || Target == nullptr || !IsValid(Target) || !Target->IsValidLowLevel())
	{
		return 0.0f;
	}
	
	return Target->GetVelocity().Size() * 0.036f;
}

//Looks at our target, and may fire if specified.
bool UMABotAIComponent::AimAtTarget(bool FireWeapon)
{
	if (AIState.CurrentTarget == nullptr || ParentCharacter == nullptr || ParentCharacter->GetController() == nullptr 
		|| AIState.CurrentTarget->GetHealth() == 0 || ParentCharacter->Weapon == nullptr )
	{
		if (ParentCharacter != nullptr)
		{
			ParentCharacter->SetTrigger(0, false);
		}
		
		return false;
	}

	//which weapon we want to use determines how
	FString WeaponClassName = ParentCharacter->Weapon->GetName();
	float ProjectileSpeed = 0.0f;
	float Inheritance = 0.0f;
	bool bIsChaingun = false;
	//todo-emallon fetch these from the class itself
	if (WeaponClassName.Contains("RingLauncher"))
	{
		ProjectileSpeed = 6500.0f;
		Inheritance = 0.5f;
	}

	if (WeaponClassName.Contains("Chaingun"))
	{
		ProjectileSpeed = 52500.0f;
		Inheritance = 1.0f;
		bIsChaingun = true;
	}

	bool bShouldFireWeapon = FireWeapon && BotConfig.bBotShoots;
	//first check if we should be firing. Generally dont want to fire TOO much, particularly on lower difficulty bots, as it gets overpowering
	float TimeSinceLastShot = ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastShot;
	if (!bIsChaingun)
	{
		switch (AccuracyLevel)
		{
		case(EBotAccuracyLevels::Horrible):
			if (TimeSinceLastShot < 6.0f)
			{
				bShouldFireWeapon = false;
			}
			break;
		case(EBotAccuracyLevels::Decent):
			if (TimeSinceLastShot < 4.0f)
			{
				bShouldFireWeapon = false;
			}
			break;
		case(EBotAccuracyLevels::Good):
			if (TimeSinceLastShot < 2.0f)
			{
				bShouldFireWeapon = false;
			}
			break;
		}
	}
	else {
		switch (AccuracyLevel)
		{
		case(EBotAccuracyLevels::Horrible):
			if (ParentCharacter->Weapon->Heat > 0.1f)
			{
				bShouldFireWeapon = false;
			}
			break;
		case(EBotAccuracyLevels::Decent):
			if (ParentCharacter->Weapon->Heat > 0.2f)
			{
				bShouldFireWeapon = false;
			}
			break;
		case(EBotAccuracyLevels::Good):
			if (ParentCharacter->Weapon->Heat > 0.4f)
			{
				bShouldFireWeapon = false;
			}
			break;
		}
	}
	if (ParentCharacter->Weapon->CurrentState != EMAWeaponActivity::WEAP_Idle || ParentCharacter->Weapon->StateTimeElapsed < ParentCharacter->Weapon->ReloadTime)
	{
		bShouldFireWeapon = false;
	}
	FVector TargetLoc = AIState.CurrentTarget->GetActorLocation();
	FVector ThisPawnLoc = ParentCharacter->GetActorLocation();
	FVector VectorToTarget = TargetLoc - ThisPawnLoc;
	FRotator Rot = UKismetMathLibrary::MakeRotFromXZ(VectorToTarget.GetSafeNormal(), ParentCharacter->GetActorUpVector());

	//We don't want bots changing where they are aiming every tick, that makes them spaz out. CHoose how much they are off by every 1 seconds and stick to it.
	if (ParentCharacter->GetWorld()->GetTimeSeconds() - TimeOfLastAimpointChange > 1.0f)
	{
		TimeOfLastAimpointChange = ParentCharacter->GetWorld()->GetTimeSeconds();
		float AddPitch = FMath::RandRange(0, 1) == 0 ? 1 : -1;
		float AddYaw = FMath::RandRange(0, 1) == 0 ? 1 : -1;
		RandomPitchSkew = 0.0f;
		RandomYawSkew = 0.0f;
		RandomProjectilePropertiesSkew = 1.0f;
		switch (AccuracyLevel)
		{
		case(EBotAccuracyLevels::Horrible):
			//for terrible bots, always make them aim actively badly almost all the time
			RandomProjectilePropertiesSkew = FMath::RandRange(0.5f, 1.5f);
			if (FMath::RandRange(0, 5) != 0)
			{
				//here we take the correct aim and always add or subtract 15-30 degrees, meaning they can't possibly hit unless super close.
				RandomPitchSkew += FMath::RandRange(15.0f, 30.0f) * AddPitch;
				RandomYawSkew += FMath::RandRange(15.0f, 30.0f) * AddYaw;
			}
			else {
				//here they still skew randomly, but if they get 0,0 (or small numbers) for skew, they can actually hit.
				RandomPitchSkew += FMath::RandRange(-25.0f, 25.0f);
				RandomYawSkew += FMath::RandRange(-15.0f, 15.0f);
			}
			break;
		case(EBotAccuracyLevels::Decent):
			//for decent bots, we have them be at least a little bad all the time, and more bad much of the time.
			if (FMath::RandRange(0, 1) == 0)
			{
				RandomProjectilePropertiesSkew = FMath::RandRange(0.2f, 1.5f);
			}
			if (FMath::RandRange(0, 1) != 0)
			{
				RandomPitchSkew += FMath::RandRange(15.0f, 25.0f) * AddPitch;
				RandomYawSkew += FMath::RandRange(15.0f, 25.0f) * AddYaw;
			}
			else {
				RandomPitchSkew += FMath::RandRange(-20.0f, 20.0f);
				RandomYawSkew += FMath::RandRange(-20.0f, 20.0f);
			}

			break;
		case(EBotAccuracyLevels::Good):
			//Good bots are off 50% of the time but by less, 25% of the time pretty close aim, and 12.5% perfectly accurate
			if (FMath::RandRange(0, 1) == 0)
			{
				RandomProjectilePropertiesSkew = FMath::RandRange(0.5f, 1.5f);
			}
			if (FMath::RandRange(0, 1) == 0)
			{
				RandomPitchSkew += FMath::RandRange(15.0f, 35.0f) * AddPitch;
				RandomYawSkew += FMath::RandRange(10.0, 30.0f) * AddYaw;
			}
			else if (FMath::RandRange(0, 1) == 0) {
				RandomPitchSkew += FMath::RandRange(-15.0f, 15.0f);
				RandomYawSkew += FMath::RandRange(-15.0f, 15.0f);
			}
			break;
		}
		//MAX/Perfect aim bot have no random pitch or yaw skew, so they aren't in this switch.
	}
	RandomYawSkew = FMath::Clamp(RandomYawSkew, -80.0f, 80.0f);
	RandomPitchSkew = FMath::Clamp(RandomPitchSkew, -80.0f, 80.0f);

	float ProjectileSkew = bShouldFireWeapon ? RandomProjectilePropertiesSkew : 1.0;
	FVector AimSpot = GetWeaponAimLocation(AIState.CurrentTarget, ProjectileSpeed * ProjectileSkew, Inheritance * ProjectileSkew);
	if (AimSpot == FVector::ForwardVector)
	{
		return false;
	}
	FRotator AimRot = AimSpot.ToOrientationRotator();

	if (!bShouldFireWeapon)
	{
		//if we just shot at something, we want to look at what we shot at, not at our move target, and then look back over time.
		//0s since last shot = look at shot, full skew 3-0=3/3=1
		//3s or greater - 0 skew 3-3=0/3=0, 3-1.5=1.5/3=.5
		float SkewFactor = (3.0f - FMath::Min(TimeSinceLastShot, 3.0f)) / 3.0f;
		AimRot.Pitch += RandomPitchSkew * SkewFactor;
		AimRot.Yaw += RandomYawSkew * SkewFactor;

		ParentCharacter->GetController()->SetControlRotation(AimRot);
		FRotator ActorRot = AimRot;
		ActorRot.Roll = 0.0f;
		ActorRot.Pitch = 0.0f;
		ParentCharacter->SetActorRotation(ActorRot);
	}
	
	//alter our spot so we miss depending on how bad our aim is
	AimRot.Pitch += RandomPitchSkew;
	AimRot.Yaw += RandomYawSkew;

	//check if we can actually still see our target.
	FHitResult HitResult;
	GetWorld()->LineTraceSingleByObjectType(
		OUT HitResult,
		ParentCharacter->GetActorLocation(),
		ThisPawnLoc + AimSpot,
		FCollisionObjectQueryParams(ECollisionChannel::ECC_OverlapAll_Deprecated),
		FCollisionQueryParams()
	);
	bool bCanSeeTarget = false;
	float DistanceToTarget = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), AimSpot);
	float DistanceToIntersectPoint = DistanceBetweenTargets(ParentCharacter->GetActorLocation(), HitResult.Location);

	if ((DistanceToIntersectPoint - 100.0f) > DistanceToTarget)
	{
		bCanSeeTarget = false;
		ParentCharacter->SetTrigger(0, false);
		return false;
	}
	else {
		bCanSeeTarget = true;
	}

	if (bShouldFireWeapon && bCanSeeTarget)
	{
		//we don't want to snap to target, but move more smoothly over there.
		//should control based on delta T
		FRotator FinalAimPoint = FMath::Lerp(ParentCharacter->GetController()->GetControlRotation(), AimRot, 0.1f);
		float AimAtAngle = FMath::RadiansToDegrees(acosf(FVector::DotProduct(ParentCharacter->GetController()->GetControlRotation().Vector(), FinalAimPoint.Vector())));
		
		ParentCharacter->GetController()->SetControlRotation(FinalAimPoint);
		FRotator ActorRot = FinalAimPoint;
		ActorRot.Roll = 0.0f;
		ActorRot.Pitch = 0.0f;
		ParentCharacter->SetActorRotation(ActorRot);

		//ensure that now that we have decided to shoot, we follow through with the shot
		AIState.bPendingWeaponFire = true;

		//only actually shoot if we are pretty close to our desired aim point for disc.
		//chain just start spewing
		if (AimAtAngle < 0.05f || bIsChaingun)
		{
			ParentCharacter->SetTrigger(0, true);
			TimeOfLastShot = ParentCharacter->GetWorld()->GetTimeSeconds();
			AIState.bPendingWeaponFire = false;
		}
	}

	return true;
}


void UMABotAIComponent::OnPawnSeen(APawn* SeenPawn)
{
	if (SeenPawn == nullptr || !IsValid(SeenPawn) )
	{
		return;
	}

	if (AMACharacter* SeenCharacter = Cast<AMACharacter>(SeenPawn))
	{
		if (SeenCharacter->GetTeamId() != ParentCharacter->GetTeamId() && !SeenCharacter->IsPendingKill())
		{
			//add the newly seen character and update their last seen time.
			RecentlySeenTargets.Add(SeenCharacter, ParentCharacter->GetWorld()->GetTimeSeconds());
		}
	}	
}

void UMABotAIComponent::OnDied()
{
	bIsJetting = false;
	AIState.RouteState = EAIRouteState::NoRouteSelected;
	AIState.CurrentTarget = nullptr;
	bIsDead = true;
	RecentlySeenTargets.Reset();
}

void UMABotAIComponent::OnSpawn()
{
	bIsDead = false;
	TimeOfLastSpawn = GetWorld()->GetTimeSeconds();
	TimeOfLastMovementChange = GetWorld()->GetTimeSeconds();
	AIState.RouteStartLocation = FVector::ZeroVector;
}

void UMABotAIComponent::PossibleTargetDied(AMACharacter* Target)
{
	if(AIState.CurrentTarget == Target)
	{
		AIState.CurrentTarget = nullptr;
	}
	TMap<AMACharacter*, float> ValidRecentlySeenTargets;
	for (auto Element : RecentlySeenTargets)
	{
		float WorldTimeLastSeen = Element.Value; //was working on giving memory to bots again
		if (Element.Key != nullptr && Element.Key && IsValid(Element.Key)
			&& Element.Key != Target)
		{
			ValidRecentlySeenTargets.Add(Element);
		}
	}
	RecentlySeenTargets = ValidRecentlySeenTargets;
}
float UMABotAIComponent::DistanceToTarget(AMACharacter* Target)
{
	if (Target == nullptr)
	{
		return 9999999.0f;
	}
	return DistanceBetweenTargets(ParentCharacter->GetActorLocation(), Target->GetActorLocation());
}

float UMABotAIComponent::DistanceBetweenTargets(FVector MyLocation, FVector TargetLocation)
{
	FVector LocationDifferenceVec = MyLocation - TargetLocation;
	float SeenDistance = FMath::Abs(LocationDifferenceVec.Size());
	return SeenDistance;
}

float UMABotAIComponent::HeightAbove(FVector TargetLocation)
{
	return ParentCharacter->GetActorLocation().Z - TargetLocation.Z;
}

FVector UMABotAIComponent::GetWeaponAimLocation(APawn* Target, float ProjectileSpeed, float Inheritance)
{
	if (Target == nullptr)
	{
		return FVector::ForwardVector;
	}
	//disc inher 50% all directions
	FVector OriginatingLoc = ParentCharacter->GetActorLocation();
	FVector TargetLoc = Target->GetActorLocation();
	FVector OriginatorVelocity = ParentCharacter->GetVelocity();
	FVector DiscVelocity = OriginatorVelocity * Inheritance;
	FVector TargetVelocity = Target->GetVelocity();
	FVector AdjustedTargetVelocity = TargetVelocity - DiscVelocity;
	float discSpeed = ProjectileSpeed; //6500.0f
	return PredictiveAim(OriginatingLoc, discSpeed, TargetLoc, AdjustedTargetVelocity, 0);
}

FVector UMABotAIComponent::PredictiveAim(FVector muzzlePosition, float projectileSpeed, FVector targetPosition, FVector targetVelocity, float gravity) {

	//adapted from https://www.gamasutra.com/blogs/KainShin/20090515/83954/Predictive_Aim_Mathematics_for_AI_Targeting.php

	//Much of this is geared towards reducing floating point precision errors
	float projectileSpeedSq = projectileSpeed * projectileSpeed;
	float targetSpeedSq = targetVelocity.Size() * targetVelocity.Size(); //doing this instead of self-multiply for maximum accuracy
	float targetSpeed = targetVelocity.Size();
	FVector targetToMuzzle = muzzlePosition - targetPosition;
	float targetToMuzzleDistSq = targetToMuzzle.Size() * targetToMuzzle.Size(); //doing this instead of self-multiply for maximum accuracy
	float targetToMuzzleDist = targetToMuzzle.Size();
	FVector targetToMuzzleDir = targetToMuzzle;
	targetToMuzzleDir.Normalize();


	float groundHeight = GetHeightAboveGround(targetPosition, false);

	if (targetPosition.Z - groundHeight < 600)
	{
		targetPosition.Z = groundHeight;
	}

	//Law of Cosines: A*A + B*B - 2*A*B*cos(theta) = C*C
	//A is distance from muzzle to target (known value: targetToMuzzleDist)
	//B is distance traveled by target until impact (targetSpeed * t)
	//C is distance traveled by projectile until impact (projectileSpeed * t)
	float cosTheta = (targetSpeedSq > 0)
		? FVector::DotProduct(targetToMuzzleDir, targetVelocity.GetSafeNormal())
		: 1.0f;

	bool validSolutionFound = true;
	float t;
	if (FMath::IsNearlyEqual(projectileSpeedSq, targetSpeedSq))
	{
		//a = projectileSpeedSq - targetSpeedSq = 0
		//We want to avoid div/0 that can result from target and projectile traveling at the same speed
		//We know that C and B are the same length because the target and projectile will travel the same distance to impact
		//Law of Cosines: A*A + B*B - 2*A*B*cos(theta) = C*C
		//Law of Cosines: A*A + B*B - 2*A*B*cos(theta) = B*B
		//Law of Cosines: A*A - 2*A*B*cos(theta) = 0
		//Law of Cosines: A*A = 2*A*B*cos(theta)
		//Law of Cosines: A = 2*B*cos(theta)
		//Law of Cosines: A/(2*cos(theta)) = B
		//Law of Cosines: 0.5f*A/cos(theta) = B
		//Law of Cosines: 0.5f * targetToMuzzleDist / cos(theta) = targetSpeed * t
		//We know that cos(theta) of zero or less means there is no solution, since that would mean B goes backwards or leads to div/0 (infinity)
		if (cosTheta > 0)
		{
			t = 0.5f * targetToMuzzleDist / (targetSpeed * cosTheta);
		}
		else
		{
			validSolutionFound = false;
			t = FMath::RandRange(1, 5);
		}
	}
	else
	{
		//Quadratic formula: Note that lower case 'a' is a completely different derived variable from capital 'A' used in Law of Cosines (sorry):
		//t = [ -b � Sqrt( b*b - 4*a*c ) ] / (2*a)
		float a = projectileSpeedSq - targetSpeedSq;
		float b = 2.0f * targetToMuzzleDist * targetSpeed * cosTheta;
		float c = -targetToMuzzleDistSq;
		float discriminant = b * b - 4.0f * a * c;

		if (discriminant < 0)
		{
			//Square root of a negative number is an imaginary number (NaN)
			//Special thanks to Rupert Key (Twitter: @Arakade) for exposing NaN values that occur when target speed is faster than or equal to projectile speed
			validSolutionFound = false;
			t = FMath::RandRange(1, 5);
		}
		else
		{
			//a will never be zero because we protect against that with "if (Mathf.Approximately(projectileSpeedSq, targetSpeedSq))" above
			float uglyNumber = FMath::Sqrt(discriminant);
			float t0 = 0.5f * (-b + uglyNumber) / a;
			float t1 = 0.5f * (-b - uglyNumber) / a;
			//Assign the lowest positive time to t to aim at the earliest hit
			t = FMath::Min(t0, t1);
			if (t < SMALL_NUMBER)
			{
				t = FMath::Max(t0, t1);
			}

			if (t < SMALL_NUMBER)
			{
				//Time can't flow backwards when it comes to aiming.
				//No real solution was found, take a wild shot at the target's future location
				validSolutionFound = false;
				t = FMath::RandRange(1, 5);
			}
		}
	}

	//Vb = Vt - 0.5*Ab*t + [(Pti - Pbi) / t]
	FVector projectileVelocity = targetVelocity + (-targetToMuzzle / t);
	if (!validSolutionFound)
	{
		//PredictiveAimWildGuessAtImpactTime gives you a t that will not result in impact
		// Which means that all that math that assumes projectileSpeed is enough to impact at time t breaks down
		// In this case, we simply want the direction to shoot to make sure we
		// don't break the gameplay rules of the cannon's capabilities aside from gravity compensation
		projectileVelocity = projectileSpeed * projectileVelocity.GetSafeNormal();
	}

	//this is drop, can be used for nades once we implement them
	//if (!Mathf.Approximately(gravity, 0))
	//{
	//	//projectileSpeed passed in is a constant that assumes zero gravity.
	//	//By adding gravity as projectile acceleration, we are essentially breaking real world rules by saying that the projectile
	//	// gets additional gravity compensation velocity for free
	//	//We want netFallDistance to match the net travel distance caused by gravity (whichever direction gravity flows)
	//	float netFallDistance = (t * projectileVelocity).Z;
	//	//d = Vi*t + 0.5*a*t^2
	//	//Vi*t = d - 0.5*a*t^2
	//	//Vi = (d - 0.5*a*t^2)/t
	//	//Remember that gravity is a positive number in the down direction, the stronger the gravity, the larger gravityCompensationSpeed becomes
	//	float gravityCompensationSpeed = (netFallDistance + 0.5f * gravity * t * t) / t;
	//	projectileVelocity.Z = gravityCompensationSpeed;
	//}

	//FOR CHECKING ONLY (valid only if gravity is 0)...
	//float calculatedprojectilespeed = projectileVelocity.magnitude;
	//bool projectilespeedmatchesexpectations = (projectileSpeed == calculatedprojectilespeed);
	//...FOR CHECKING ONLY

	return projectileVelocity;
}


float UMABotAIComponent::GetHeightAboveGround(FVector Point, bool bDrawDebugLines)
{
	UWorld* World = ParentCharacter->GetWorld() ;

	if (World)
	{
		FVector StartLocation{ Point.X, Point.Y, 10000 };    // Raytrace starting point.
		FVector EndLocation{ Point.X, Point.Y, -10000 };            // Raytrace end point.

		// Raytrace for overlapping actors.
		FHitResult HitResult;
		World->LineTraceSingleByObjectType(
			OUT HitResult,
			StartLocation,
			EndLocation,
			FCollisionObjectQueryParams(ECollisionChannel::ECC_WorldStatic),
			FCollisionQueryParams()
		);

		// Draw debug line.
		if (bDrawDebugLines  && bBotDebugMode)
		{
			FColor LineColor;

			if (HitResult.GetActor()) LineColor = FColor::Red;
			else LineColor = FColor::Green;

			DrawDebugLine(
				World,
				StartLocation,
				EndLocation,
				LineColor,
				true,
				1.f,
				0.f,
				10.f
			);
		}

		// Return distance from actor to ground.
		if (HitResult.GetActor()) return Point.Z - HitResult.ImpactPoint.Z;
	}

	return 0;
}

void UMABotAIComponent::ClientDrawDebugLine_Implementation(FVector const& LineStart, FVector const& LineEnd, FColor const& Color, float LifeTime)
{
	DrawDebugLine(
		ParentCharacter->GetWorld(),
		LineStart,
		LineEnd,
		Color,
		true,
		10.f,
		ESceneDepthPriorityGroup::SDPG_World,
		10.f
	);
}


