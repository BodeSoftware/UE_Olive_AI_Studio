// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/PCGIR.h"

class UPCGGraph;
class UPCGNode;
class UPCGPin;
class UPCGSettings;
enum class EPCGDataType : uint32;

/**
 * FOlivePCGReader
 *
 * Reads UPCGGraph assets and produces FOliveIRPCGGraph intermediate representation.
 * Singleton pattern matching FOliveBehaviorTreeReader.
 *
 * Usage:
 *   TOptional<FOliveIRPCGGraph> IR = FOlivePCGReader::Get().ReadPCGGraph("/Game/PCG/MyGraph");
 */
class OLIVEAIEDITOR_API FOlivePCGReader
{
public:
	/** Get the singleton instance */
	static FOlivePCGReader& Get();

	/**
	 * Read a PCG graph by asset path
	 * @param AssetPath Content path to the PCG graph
	 * @return IR representation, or unset on failure
	 */
	TOptional<FOliveIRPCGGraph> ReadPCGGraph(const FString& AssetPath);

	/**
	 * Read a PCG graph from an existing pointer
	 * @param Graph The PCG graph to read
	 * @return IR representation, or unset on failure
	 */
	TOptional<FOliveIRPCGGraph> ReadPCGGraph(const UPCGGraph* Graph);

private:
	FOlivePCGReader() = default;
	~FOlivePCGReader() = default;
	FOlivePCGReader(const FOlivePCGReader&) = delete;
	FOlivePCGReader& operator=(const FOlivePCGReader&) = delete;

	/** Load a PCG graph asset by path */
	UPCGGraph* LoadPCGGraph(const FString& AssetPath) const;

	/** Serialize a single PCG node to IR */
	FOliveIRPCGNode SerializeNode(const UPCGNode* Node, const FString& NodeId) const;

	/** Serialize a pin to IR */
	FOliveIRPCGPin SerializePin(const UPCGPin* Pin) const;

	/** Map engine data type enum to IR data type */
	EOliveIRPCGDataType MapDataType(EPCGDataType EngineType) const;

	/** Serialize settings properties via reflection */
	void SerializeSettings(const UPCGSettings* Settings, TMap<FString, FString>& OutSettings) const;
};
