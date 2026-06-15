#pragma once

#include "Containers/UnrealString.h"

namespace BlueprintReaderPaths {

// Strip the object suffix to convert /Game/Foo/Bar.Bar -> /Game/Foo/Bar. The
// wire format consistently uses package paths, not object paths. Shared by the
// commandlet + wire-json TUs (it was anon-namespace-duplicated in both).
inline FString ToPackagePath(const FString& In)
{
	int32 DotIdx = INDEX_NONE;
	if (In.FindChar(TEXT('.'), DotIdx))
	{
		return In.Left(DotIdx);
	}
	return In;
}

}    // namespace BlueprintReaderPaths
