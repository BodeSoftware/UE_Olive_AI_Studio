// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonIR.h"
#include "PCGIR.generated.h"

/**
 * PCG data type
 */
UENUM(BlueprintType)
enum class EOliveIRPCGDataType : uint8
{
	Point,
	Spline,
	Surface,
	Volume,
	Landscape,
	Primitive,
	Concrete,      // Actual mesh instances, etc.
	Attribute,
	Param,
	Spatial,
	LandscapeSpline,
	PolyLine,
	Texture,
	RenderTarget,
	DynamicMesh,
	Composite,
	Any,
	Unknown
};

/**
 * PCG pin definition
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPCGPin
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	EOliveIRPCGDataType DataType = EOliveIRPCGDataType::Unknown;

	UPROPERTY()
	bool bIsInput = true;

	UPROPERTY()
	bool bAllowMultipleConnections = false;

	/** Connected pins in format "node_id.pin_name" */
	UPROPERTY()
	TArray<FString> Connections;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPCGPin FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * PCG node
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPCGNode
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	/** Node type/class (e.g., PCGPointGenerator, PCGSurfaceSampler) */
	UPROPERTY()
	FString NodeType;

	UPROPERTY()
	FString Title;

	/** Editor node position */
	UPROPERTY()
	int32 PositionX = 0;

	UPROPERTY()
	int32 PositionY = 0;

	/** Optional node comment */
	UPROPERTY()
	FString Comment;

	UPROPERTY()
	TArray<FOliveIRPCGPin> InputPins;

	UPROPERTY()
	TArray<FOliveIRPCGPin> OutputPins;

	/** Node settings */
	UPROPERTY()
	TMap<FString, FString> Settings;

	/** Whether this node is enabled */
	UPROPERTY()
	bool bEnabled = true;

	/** Debug visualization settings */
	UPROPERTY()
	bool bDebug = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPCGNode FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * PCG edge connecting two pins
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPCGEdge
{
	GENERATED_BODY()

	UPROPERTY()
	FString SourceNodeId;

	UPROPERTY()
	FString SourcePinName;

	UPROPERTY()
	FString TargetNodeId;

	UPROPERTY()
	FString TargetPinName;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPCGEdge FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * PCG graph input/output configuration
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPCGGraphInterface
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FOliveIRPCGPin> InputPins;

	UPROPERTY()
	TArray<FOliveIRPCGPin> OutputPins;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPCGGraphInterface FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * PCG graph asset IR
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRPCGGraph
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Path;

	/** Whether this is a subgraph */
	UPROPERTY()
	bool bIsSubgraph = false;

	/** Graph interface */
	UPROPERTY()
	FOliveIRPCGGraphInterface Interface;

	/** All nodes in the graph */
	UPROPERTY()
	TArray<FOliveIRPCGNode> Nodes;

	/** Input node ID */
	UPROPERTY()
	FString InputNodeId;

	/** Output node ID */
	UPROPERTY()
	FString OutputNodeId;

	/** Explicit edge list */
	UPROPERTY()
	TArray<FOliveIRPCGEdge> Edges;

	/** Subgraphs referenced by this graph */
	UPROPERTY()
	TArray<FString> SubgraphPaths;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRPCGGraph FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
