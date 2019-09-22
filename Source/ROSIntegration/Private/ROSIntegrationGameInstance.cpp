#include "ROSIntegrationGameInstance.h"
#include "RI/Topic.h"
#include "RI/Service.h"
#include "ROSTime.h"
#include "rosgraph_msgs/Clock.h"
#include "Misc/App.h"


static void MarkAllROSObjectsAsDisconnected()
{
	for (TObjectIterator<UTopic> It; It; ++It)
	{
		UTopic* Topic = *It;

		Topic->MarkAsDisconnected();  
	}
	for (TObjectIterator<UService> It; It; ++It)
	{
		UService* Service = *It;

		Service->MarkAsDisconnected();   
	}
}

void UROSIntegrationGameInstance::Init()
{
	if (bConnectToROS)
	{
		bool resLock = initMutex_.TryLock(); 
		if (!resLock)
		{
			UE_LOG(LogROS, Display, TEXT("UROSIntegrationGameInstance::Init() - already connection to ROS bridge!"));
			return; // EXIT POINT!
		}

		FLocker locker(&initMutex_);

		UE_LOG(LogROS, Display, TEXT("UROSIntegrationGameInstance::Init() - connecting to ROS bridge..."));

		FROSTime::SetUseSimTime(false);

		if (ROSIntegrationCore)
		{
			UROSIntegrationCore* oldRosCore = ROSIntegrationCore;
			ROSIntegrationCore = nullptr;
			oldRosCore->ConditionalBeginDestroy();
		}

		ROSIntegrationCore = NewObject<UROSIntegrationCore>(UROSIntegrationCore::StaticClass()); // ORIGINAL 
		bIsConnected = ROSIntegrationCore->Init(ROSBridgeServerHost, ROSBridgeServerPort);

		if (!bTimerSet)
		{
			bTimerSet = true; 
			GetTimerManager().SetTimer(TimerHandle_CheckHealth, this, &UROSIntegrationGameInstance::CheckROSBridgeHealth, 1.0f, true, 5.0f);
		}

		if (bIsConnected)
		{
			UWorld* CurrentWorld = GetWorld();
			if (CurrentWorld)
			{
				ROSIntegrationCore->SetWorld(CurrentWorld);
				ROSIntegrationCore->InitSpawnManager();
			}
			else
			{
				UE_LOG(LogROS, Display, TEXT("World not available in UROSIntegrationGameInstance::Init()!"));
			}
		}
		else if (!bReconnect)
		{
			UE_LOG(LogROS, Error, TEXT("Failed to connect to server %s:%u. Please make sure that your rosbridge is running."), *ROSBridgeServerHost, ROSBridgeServerPort);
		}

		if (bSimulateTime)
		{
			FApp::SetFixedDeltaTime(FixedUpdateInterval);
			FApp::SetUseFixedTimeStep(bUseFixedUpdateInterval);

			// tell ROSIntegration to use simulated time
			FROSTime now = FROSTime::Now();
			FROSTime::SetUseSimTime(true);
			FROSTime::SetSimTime(now);

			FWorldDelegates::OnWorldTickStart.AddUObject(this, &UROSIntegrationGameInstance::OnWorldTickStart);

			ClockTopic = NewObject<UTopic>(UTopic::StaticClass()); // ORIGINAL

			ClockTopic->Init(ROSIntegrationCore, FString(TEXT("/clock")), FString(TEXT("rosgraph_msgs/Clock")), 3);

			ClockTopic->Advertise();
		}
	}
}

void UROSIntegrationGameInstance::CheckROSBridgeHealth()
{
	if (!bCheckHealth) return; 

	if (bIsConnected && ROSIntegrationCore->IsHealthy())
	{
		return;
	}

	if (bIsConnected)
	{
		UE_LOG(LogROS, Error, TEXT("Connection to rosbridge %s:%u was interrupted."), *ROSBridgeServerHost, ROSBridgeServerPort);
	}

	// reconnect again
	bIsConnected = false;
	bReconnect = true;
	Init();
	bReconnect = false;

	// tell everyone (Topics, Services, etc.) they lost connection and should stop any interaction with ROS for now.
	MarkAllROSObjectsAsDisconnected();

	if (!bIsConnected)
	{
		return; // Let timer call this method again to retry connection attempt
	}

	// tell everyone (Topics, Services, etc.) they can try to reconnect (subscribe and advertise)
	{
		for (TObjectIterator<UTopic> It; It; ++It)
		{
			UTopic* Topic = *It;

			bool success = Topic->Reconnect(ROSIntegrationCore);
			if (!success)
			{
				bIsConnected = false;
				UE_LOG(LogROS, Error, TEXT("Unable to re-establish topic %s."), *Topic->GetDetailedInfo());
			}
		}
		for (TObjectIterator<UService> It; It; ++It)
		{
			UService* Service = *It;

			bool success = Service->Reconnect(ROSIntegrationCore);
			if (!success)
			{
				bIsConnected = false;
				UE_LOG(LogROS, Error, TEXT("Unable to re-establish service %s."), *Service->GetDetailedInfo());
			}
		}
	}

	UE_LOG(LogROS, Display, TEXT("Successfully reconnected to rosbridge %s:%u."), *ROSBridgeServerHost, ROSBridgeServerPort);
}

// N.B.: from log, first comes Shutdown() and then BeginDestroy()
void UROSIntegrationGameInstance::Shutdown()
{
	UE_LOG(LogROS, Display, TEXT("ROS Game Instance - shutdown start"));
	if (bConnectToROS)
	{
		if(bTimerSet) GetTimerManager().ClearTimer(TimerHandle_CheckHealth);

		if (bSimulateTime)
		{
			FWorldDelegates::OnWorldTickStart.RemoveAll(this);
		}

		MarkAllROSObjectsAsDisconnected(); // moved here from UROSIntegrationGameInstance::BeginDestroy()

		UE_LOG(LogROS, Display, TEXT("ROS Game Instance - shutdown done"));
	}
}

void UROSIntegrationGameInstance::BeginDestroy()
{
	// tell everyone (Topics, Services, etc.) they should stop any interaction with ROS.
	if (bConnectToROS) 
	{
		UE_LOG(LogROS, Display, TEXT("ROS Game Instance - begin destroy - start"));

		//MarkAllROSObjectsAsDisconnected();  // moved in UROSIntegrationGameInstance::Shutdown()

		//ROSIntegrationCore->ConditionalBeginDestroy();
		//ROSIntegrationCore = nullptr; // this brings troubles (reported from crash window on editor closing)

		//ClockTopic->ConditionalBeginDestroy(); // this brings troubles (reported from crash window on editor closing)
		
		//if (GetWorld()) GetWorld()->ForceGarbageCollection(true);  // this brings troubles (reported from crash window on editor closing)
	}

	Super::BeginDestroy();

	UE_LOG(LogROS, Display, TEXT("ROS Game Instance - begin destroy - done"));
}

void UROSIntegrationGameInstance::OnWorldTickStart(ELevelTick TickType, float DeltaTime)
{
	if (bSimulateTime)
	{
		FApp::SetFixedDeltaTime(FixedUpdateInterval);
		FApp::SetUseFixedTimeStep(bUseFixedUpdateInterval);

		FROSTime now = FROSTime::Now();

		// advance ROS time
		unsigned long seconds = (unsigned long)DeltaTime;
		unsigned long long nanoseconds = (unsigned long long)(DeltaTime * 1000000000ul);
		unsigned long nanoseconds_only = nanoseconds - (seconds * 1000000000ul);

		now._Sec += seconds;
		now._NSec += nanoseconds_only;

		if (now._NSec >= 1000000000ul)
		{
			now._Sec += 1;
			now._NSec -= 1000000000ul;
		}

		// internal update for ROSIntegration
		FROSTime::SetSimTime(now);

		// send /clock topic to let everyone know what time it is...
		TSharedPtr<ROSMessages::rosgraph_msgs::Clock> ClockMessage(new ROSMessages::rosgraph_msgs::Clock(now));
		ClockTopic->Publish(ClockMessage);
	}
}

