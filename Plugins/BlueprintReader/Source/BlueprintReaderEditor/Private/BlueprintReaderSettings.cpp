#include "BlueprintReaderSettings.h"

#define LOCTEXT_NAMESPACE "BlueprintReader"

#if WITH_EDITOR
FText UBlueprintReaderSettings::GetSectionText() const
{
	return LOCTEXT("BlueprintReaderMcpSettings", "BlueprintReader MCP");
}

FText UBlueprintReaderSettings::GetSectionDescription() const
{
	return LOCTEXT("BlueprintReaderMcpSettingsDesc",
		"Configuration for the out-of-process BlueprintReader MCP server "
		"and in-editor live TCP listener. Settings here are written to "
		"<Project>/Saved/bp-reader-settings.json so the MCP server picks "
		"them up at startup. Environment variables (BP_READER_*) override "
		"these values when set.");
}
#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
