// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveTransactionManager.h"

DEFINE_LOG_CATEGORY(LogOliveTransaction);

FOliveTransactionManager& FOliveTransactionManager::Get()
{
	static FOliveTransactionManager Instance;
	return Instance;
}

int32 FOliveTransactionManager::BeginTransaction(const FText& Description)
{
	FScopeLock Lock(&TransactionLock);

	int32 TransactionId = NextTransactionId++;
	ActiveTransactions.Add(TransactionId, MakeUnique<FScopedTransaction>(Description));

	UE_LOG(LogOliveTransaction, Verbose, TEXT("Begin transaction %d: %s"), TransactionId, *Description.ToString());

	return TransactionId;
}

void FOliveTransactionManager::EndTransaction(int32 TransactionId)
{
	FScopeLock Lock(&TransactionLock);

	if (ActiveTransactions.Contains(TransactionId))
	{
		UE_LOG(LogOliveTransaction, Verbose, TEXT("End transaction %d"), TransactionId);
		ActiveTransactions.Remove(TransactionId);
	}
	else
	{
		UE_LOG(LogOliveTransaction, Warning, TEXT("Attempted to end non-existent transaction %d"), TransactionId);
	}
}

void FOliveTransactionManager::CancelTransaction(int32 TransactionId)
{
	FScopeLock Lock(&TransactionLock);

	if (ActiveTransactions.Contains(TransactionId))
	{
		UE_LOG(LogOliveTransaction, Log, TEXT("Cancel transaction %d"), TransactionId);

		// Get the transaction and cancel it
		TUniquePtr<FScopedTransaction>& Transaction = ActiveTransactions[TransactionId];
		if (Transaction.IsValid())
		{
			Transaction->Cancel();
		}

		ActiveTransactions.Remove(TransactionId);
	}
	else
	{
		UE_LOG(LogOliveTransaction, Warning, TEXT("Attempted to cancel non-existent transaction %d"), TransactionId);
	}
}

bool FOliveTransactionManager::IsTransactionActive(int32 TransactionId) const
{
	FScopeLock Lock(&TransactionLock);
	return ActiveTransactions.Contains(TransactionId);
}

int32 FOliveTransactionManager::GetActiveTransactionCount() const
{
	FScopeLock Lock(&TransactionLock);
	return ActiveTransactions.Num();
}

// FScopedOliveTransaction implementation

FOliveTransactionManager::FScopedOliveTransaction::FScopedOliveTransaction(const FText& Description)
{
	Transaction = MakeUnique<FScopedTransaction>(Description);
	UE_LOG(LogOliveTransaction, Verbose, TEXT("Scoped transaction begin: %s"), *Description.ToString());
}

FOliveTransactionManager::FScopedOliveTransaction::~FScopedOliveTransaction()
{
	if (bCancelled && Transaction.IsValid())
	{
		UE_LOG(LogOliveTransaction, Log, TEXT("Scoped transaction cancelled and rolling back"));
		Transaction->Cancel();
	}
	else
	{
		UE_LOG(LogOliveTransaction, Verbose, TEXT("Scoped transaction completed"));
	}
}

void FOliveTransactionManager::FScopedOliveTransaction::Cancel()
{
	bCancelled = true;
}
