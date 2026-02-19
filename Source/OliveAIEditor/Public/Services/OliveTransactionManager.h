// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveTransaction, Log, All);

/**
 * Transaction Manager
 *
 * Wraps all write operations in FScopedTransaction for undo support.
 * Provides scoped helper for automatic transaction management.
 */
class OLIVEAIEDITOR_API FOliveTransactionManager
{
public:
	/** Get singleton instance */
	static FOliveTransactionManager& Get();

	/**
	 * Begin a named transaction
	 * @param Description Human-readable transaction description
	 * @return Transaction ID for later reference
	 */
	int32 BeginTransaction(const FText& Description);

	/**
	 * End a transaction by ID
	 * @param TransactionId ID returned from BeginTransaction
	 */
	void EndTransaction(int32 TransactionId);

	/**
	 * Cancel/rollback a transaction
	 * @param TransactionId ID returned from BeginTransaction
	 */
	void CancelTransaction(int32 TransactionId);

	/**
	 * Check if a transaction is currently active
	 * @param TransactionId Transaction to check
	 * @return True if transaction is still active
	 */
	bool IsTransactionActive(int32 TransactionId) const;

	/**
	 * Get the number of active transactions
	 */
	int32 GetActiveTransactionCount() const;

	/**
	 * Scoped transaction helper - automatically ends/cancels on destruction
	 */
	class OLIVEAIEDITOR_API FScopedOliveTransaction
	{
	public:
		FScopedOliveTransaction(const FText& Description);
		~FScopedOliveTransaction();

		/** Mark the transaction as cancelled - it will be rolled back on destruction */
		void Cancel();

		/** Check if this transaction was cancelled */
		bool WasCancelled() const { return bCancelled; }

		/** Get the underlying transaction (may be null if cancelled) */
		FScopedTransaction* GetTransaction() const { return Transaction.Get(); }

	private:
		TUniquePtr<FScopedTransaction> Transaction;
		bool bCancelled = false;
	};

private:
	FOliveTransactionManager() = default;

	TMap<int32, TUniquePtr<FScopedTransaction>> ActiveTransactions;
	int32 NextTransactionId = 1;
	mutable FCriticalSection TransactionLock;
};

// Convenience macro for creating scoped transactions
#define OLIVE_SCOPED_TRANSACTION(Description) \
	FOliveTransactionManager::FScopedOliveTransaction OliveTransaction(Description)
