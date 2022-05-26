/**

Snippet of code from our weapon class, for handling heat management.

*/
//Updates the weapon's heat factor, which slows fire rate based on blueprint configured values.
//the more heat a weapon has, the slower it fires
//Each shot adds heat, heat is lost by time modified by player movement speed. (faster movement = more heat loss)
//weapon model/material can hook into heat param for modified visuals (CG heats up red/orange on the barrel)
void AMAWeapon::UpdateCurrentHeat(float DeltaTime)
{
	AMAGameState* GS = UMAGameplayStatics::GetMAGameState(GetWorld());
	//Don't bother with any of this if we don't generate heat.
	//Weapon heat can be enabled/disabled via console by admin for now as well.
	if (GS->bWeaponHeatEnabled == false || HeatPerShot == 0.0f) {
		return;
	}
	float CharacterSpeed = GetMAOwner()->GetVelocity().Size() * 0.036f; //scale for KPH
	//Currently can't actually hit overheat threshold with CG, but leave this here for any other weapon that uses heat.
	//current BP values 6/29/20 - HPS - .04, Heat Loss Per Second - .1.
	float HeatLoss = (CurrentState == EMAWeaponActivity::WEAP_Overheated) ? OverheatedHeatLossPerSecond : HeatLossPerSecond;
	//speed at which dissipation outweighs heat gain while firing.
	float HeatDissapationThresholdSpeed = 110.0f;
	// you get a percentage of your expected heat loss based on player move speed.
	float WindHeatLoss = 0.25f * CharacterSpeed / HeatDissapationThresholdSpeed;
	HeatLoss = HeatLoss + WindHeatLoss;
	Heat = FMath::Max(Heat - HeatLoss * DeltaTime, 0.0f);
	//how much heat is slowing your fire rate. Reduce heat implications while spinning up.
	//Add 0.05f buffer so you are generally at 100% when moving and low heat.
	HeatFactor = FMath::Clamp((1.0f - Heat) + 0.05f, 0.0f, 1.0f);
	//GEngine->AddOnScreenDebugMessage(GetFName().GetNumber() + 27, 12.f, FColor::Yellow, FString::Printf(TEXT("Character Speed ---  %i, --- %i"), FMath::RoundToInt(CharacterSpeed), FMath::RoundToInt(100.0f * CharacterSpeed / HeatDissapationThresholdSpeed)));
	//GEngine->AddOnScreenDebugMessage(GetFName().GetNumber() + 28, 12.f, FColor::Orange, FString::Printf(TEXT("Heat ---  %i"), FMath::RoundToInt(100.0f*Heat)));
	//GEngine->AddOnScreenDebugMessage(GetFName().GetNumber() + 26, 12.f, FColor::Red, FString::Printf(TEXT("HeatFactor ---  %i"), FMath::RoundToInt(100.0f*HeatFactor)));
}

