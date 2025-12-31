// Copyright (c) 2022 YDrive Inc. All rights reserved.

#include "Widgets/WidgetManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "LevelSequence.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SDirectoryPicker.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"

#include "EasySynth.h"
#include "Widgets/WidgetStateAsset.h"

#include "HAL/PlatformProcess.h"

const FString FWidgetManager::TextureStyleColorName(TEXT("Original color textures"));
const FString FWidgetManager::TextureStyleSemanticName(TEXT("Semantic color textures"));
const FString FWidgetManager::JpegFormatName(TEXT("jpeg"));
const FString FWidgetManager::PngFormatName(TEXT("png"));
const FString FWidgetManager::ExrFormatName(TEXT("exr"));
const FIntPoint FWidgetManager::DefaultOutputImageResolution(1920, 1080);

#define LOCTEXT_NAMESPACE "FWidgetManager"

FWidgetManager::FWidgetManager() :
	OutputImageResolution(DefaultOutputImageResolution),
	OutputDirectory(FPathUtils::DefaultRenderingOutputPath()),
	CurrentSequenceIndex(-1)
{
	// Create the texture style manager and add it to the root to avoid garbage collection
	TextureStyleManager = NewObject<UTextureStyleManager>();
	check(TextureStyleManager);
	TextureStyleManager->AddToRoot();
	// Register the semantic classes updated callback
	TextureStyleManager->OnSemanticClassesUpdated().AddRaw(this, &FWidgetManager::OnSemanticClassesUpdated);

	// Create the sequence renderer and add it to the root to avoid garbage collection
	SequenceRenderer = NewObject<USequenceRenderer>();
	check(SequenceRenderer)
	SequenceRenderer->AddToRoot();
	// Register the rendering finished callback
	SequenceRenderer->OnRenderingFinished().AddRaw(this, &FWidgetManager::OnRenderingFinished);
	SequenceRenderer->SetTextureStyleManager(TextureStyleManager);

	// No need to ever release the TextureStyleManager and the SequenceRenderer,
	// as the FWidgetManager lives as long as the plugin inside the editor

	// Prepare content of the texture style checkout combo box
	TextureStyleNames.Add(MakeShared<FString>(TextureStyleColorName));
	TextureStyleNames.Add(MakeShared<FString>(TextureStyleSemanticName));

	// Prepare content of the outut image format combo box
	OutputFormatNames.Add(MakeShared<FString>(JpegFormatName));
	OutputFormatNames.Add(MakeShared<FString>(PngFormatName));
	OutputFormatNames.Add(MakeShared<FString>(ExrFormatName));

	// Initialize SemanticClassesWidgetManager
	SemanticsWidget.SetTextureStyleManager(TextureStyleManager);
}

TSharedRef<SDockTab> FWidgetManager::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	// Bind events now that the editor has finished starting up
	TextureStyleManager->BindEvents();

	// Load saved option states now, also to make sure editor is ready
	LoadWidgetOptionStates();

	// Update combo box semantic class names
	OnSemanticClassesUpdated();

	// Dynamically generate renderer target checkboxes
	TSharedRef<SScrollBox> TargetsScrollBoxes = SNew(SScrollBox);
	TMap<FRendererTargetOptions::TargetType, FText> TargetCheckBoxNames;
	TargetCheckBoxNames.Add(FRendererTargetOptions::COLOR_IMAGE, LOCTEXT("ColorImagesCheckBoxText", "Color images"));
	TargetCheckBoxNames.Add(FRendererTargetOptions::DEPTH_IMAGE, LOCTEXT("DepthImagesCheckBoxText", "Depth images"));
	TargetCheckBoxNames.Add(FRendererTargetOptions::NORMAL_IMAGE, LOCTEXT("NormalImagesCheckBoxText", "Normal images"));
	TargetCheckBoxNames.Add(FRendererTargetOptions::OPTICAL_FLOW_IMAGE, LOCTEXT("OpticalFlowImagesCheckBoxText", "Optical flow images"));
	TargetCheckBoxNames.Add(FRendererTargetOptions::SEMANTIC_IMAGE, LOCTEXT("SemanticImagesCheckBoxText", "Semantic images"));
	for (auto Element : TargetCheckBoxNames)
	{
		const FRendererTargetOptions::TargetType TargetType = Element.Key;
		const FText CheckBoxText = Element.Value;
		TargetsScrollBoxes->AddSlot()
			.Padding(2)
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				[
					SNew(SCheckBox)
					.IsChecked_Raw(this, &FWidgetManager::RenderTargetsCheckedState, TargetType)
					.OnCheckStateChanged_Raw(this, &FWidgetManager::OnRenderTargetsChanged, TargetType)
					[
						SNew(STextBlock)
						.Text(CheckBoxText)
					]
				]
				+SHorizontalBox::Slot()
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&OutputFormatNames)
					.ContentPadding(2)
					.OnGenerateWidget_Lambda(
						[](TSharedPtr<FString> StringItem)
						{ return SNew(STextBlock).Text(FText::FromString(*StringItem)); })
					.OnSelectionChanged_Raw(this, &FWidgetManager::OnOutputFormatSelectionChanged, TargetType)
					[
						SNew(STextBlock)
						.Text_Raw(this, &FWidgetManager::SelectedOutputFormat, TargetType)
					]
				]
			];
	}

	// Generate the UI
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.ContentPadding(2)
		[
			SNew(SScrollBox)
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SButton)
				.OnClicked_Raw(
					&SemanticCsvInterface,
					&FSemanticCsvInterface::OnImportSemanticClassesClicked,
					TextureStyleManager)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ImportSemanticClassesButtonText", "Import semantic classes CSV file"))
				]
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SButton)
				.OnClicked_Raw(&CameraRigRosInterface, &FCameraRigRosInterface::OnImportCameraRigClicked)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ImportCameraRigButtonText", "Import camera rig ROS JSON file"))
				]
			]
			+SScrollBox::Slot()
			.Padding(0, 2, 0, 2)
			[
				SNew(SSeparator)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SButton)
				.OnClicked_Raw(&SemanticsWidget, &FSemanticClassesWidgetManager::OnManageSemanticClassesClicked)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ManageSemanticClassesButtonText", "Manage Semantic Classes"))
				]
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SAssignNew(SemanticClassComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&SemanticClassNames)
				.ContentPadding(2)
				.OnGenerateWidget_Lambda(
					[](TSharedPtr<FString> StringItem)
					{ return SNew(STextBlock).Text(FText::FromString(*StringItem)); })
				.OnSelectionChanged_Raw(this, &FWidgetManager::OnSemanticClassComboBoxSelectionChanged)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PickSemanticClassComboBoxText", "Pick a semantic class"))
				]
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&TextureStyleNames)
				.ContentPadding(2)
				.OnGenerateWidget_Lambda(
					[](TSharedPtr<FString> StringItem)
					{ return SNew(STextBlock).Text(FText::FromString(*StringItem)); })
				.OnSelectionChanged_Raw(this, &FWidgetManager::OnTextureStyleComboBoxSelectionChanged)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PickMeshTextureStyleComboBoxText", "Pick a mesh texture style"))
				]
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PickSequencesFolderSectionTitle", "Pick sequences folder"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SDirectoryPicker)
				.Directory(SelectedSequencesFolder)
				.OnDirectoryChanged_Raw(this, &FWidgetManager::OnSequencesFolderChanged)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ChoseTargetsSectionTitle", "Chose targets to be rendered"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda(
					[this]()
					{
						const bool bChecked = SequenceRendererTargets.ExportCameraPoses();
						return bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
				.OnCheckStateChanged_Lambda(
					[this](ECheckBoxState NewState)
					{ SequenceRendererTargets.SetExportCameraPoses(NewState == ECheckBoxState::Checked); })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CameraPosesCheckBoxText", "Camera poses"))
				]
			]
			+SScrollBox::Slot()
			[
				TargetsScrollBoxes
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CustomPPMaterialSectionTitle", "Optional custom PP material render target"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.Padding(2)
				[
					SNew(SObjectPropertyEntryBox)
					.AllowedClass(UMaterial::StaticClass())
					.ObjectPath_Raw(this, &FWidgetManager::GetCustomPPMaterialPath)
					.OnObjectChanged_Raw(this, &FWidgetManager::OnCustomPPMaterialSelected)
					.AllowClear(true)
					.DisplayUseSelected(true)
					.DisplayBrowse(true)
				]
				+SHorizontalBox::Slot()
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&OutputFormatNames)
					.ContentPadding(2)
					.OnGenerateWidget_Lambda(
						[](TSharedPtr<FString> StringItem)
						{ return SNew(STextBlock).Text(FText::FromString(*StringItem)); })
					.OnSelectionChanged_Raw(this, &FWidgetManager::OnOutputFormatSelectionChanged, FRendererTargetOptions::CUSTOM_PP_MATERIAL)
					[
						SNew(STextBlock)
						.Text_Raw(this, &FWidgetManager::SelectedOutputFormat, FRendererTargetOptions::CUSTOM_PP_MATERIAL)
					]
				]
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OutputWidthText", "Output image width [px]"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSpinBox<int32>)
				.Value_Lambda([this](){ return OutputImageResolution.X; })
				.OnValueChanged_Lambda([this](const int32 NewValue){ OutputImageResolution.X = NewValue / 2 * 2; })
				.MinValue(100)
				.MaxValue(1920 * 2)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OutputHeightText", "Output image height [px]"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSpinBox<int32>)
				.Value_Lambda([this](){ return OutputImageResolution.Y; })
				.OnValueChanged_Lambda([this](const int32 NewValue){ OutputImageResolution.Y = NewValue / 2 * 2; })
				.MinValue(100)
				.MaxValue(1080 * 2)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text_Lambda(
					[this]()
					{
						return FText::Format(
							LOCTEXT("OutputAspectRatioText", "Output aspect ratio: {0}"),
							FText::AsNumber(1.0f * OutputImageResolution.X / OutputImageResolution.Y));
					})
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DepthRangeText", "Depth range [m]"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSpinBox<float>)
				.Value_Lambda([this](){ return SequenceRendererTargets.DepthRangeMeters(); })
				.OnValueChanged_Lambda(
					[this](const float NewValue){ SequenceRendererTargets.SetDepthRangeMeters(NewValue); })
				.MinValue(0.01f)
				.MaxValue(10000.0f)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OpticalFlowScaleText", "Optical flow scale coefficient"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSpinBox<float>)
				.Value_Lambda([this](){ return SequenceRendererTargets.OpticalFlowScale(); })
				.OnValueChanged_Lambda(
					[this](const float NewValue){ SequenceRendererTargets.SetOpticalFlowScale(NewValue); })
				.MinValue(1.0f)
				.MaxValue(100.0f)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OuputDirectoryText", "Ouput directory"))
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SDirectoryPicker)
				.Directory(OutputDirectory)
				.OnDirectoryChanged_Raw(this, &FWidgetManager::OnOutputDirectoryChanged)
			]
			+SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SButton)
				.IsEnabled_Raw(this, &FWidgetManager::GetIsRenderImagesEnabled)
				.OnClicked_Raw(this, &FWidgetManager::OnRenderImagesClicked)
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RenderImagesButtonText", "Render Images"))
				]
			]
		];
}

void FWidgetManager::OnSemanticClassComboBoxSelectionChanged(
	TSharedPtr<FString> StringItem,
	ESelectInfo::Type SelectInfo)
{
	if (StringItem.IsValid())
	{
		UE_LOG(LogEasySynth, Log, TEXT("%s: Semantic class selected: %s"), *FString(__FUNCTION__), **StringItem)
		TextureStyleManager->ApplySemanticClassToSelectedActors(*StringItem);
		SemanticClassComboBox->ClearSelection();
	}
}

void FWidgetManager::OnTextureStyleComboBoxSelectionChanged(
	TSharedPtr<FString> StringItem,
	ESelectInfo::Type SelectInfo)
{
	if (StringItem.IsValid())
	{
		UE_LOG(LogEasySynth, Log, TEXT("%s: Texture style selected: %s"), *FString(__FUNCTION__), **StringItem)
		if (*StringItem == TextureStyleColorName)
		{
			TextureStyleManager->CheckoutTextureStyle(ETextureStyle::COLOR);
		}
		else if (*StringItem == TextureStyleSemanticName)
		{
			TextureStyleManager->CheckoutTextureStyle(ETextureStyle::SEMANTIC);
		}
		else
		{
			UE_LOG(LogEasySynth, Error, TEXT("%s: Got unexpected texture style: %s"),
				*FString(__FUNCTION__), **StringItem);
		}
	}
}

void FWidgetManager::OnSequencesFolderChanged(const FString& Directory)
{
	SelectedSequencesFolder = Directory;
}

ECheckBoxState FWidgetManager::RenderTargetsCheckedState(const FRendererTargetOptions::TargetType TargetType) const
{
	const bool bChecked = SequenceRendererTargets.TargetSelected(TargetType);
	return bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FWidgetManager::OnRenderTargetsChanged(
	ECheckBoxState NewState,
	const FRendererTargetOptions::TargetType TargetType)
{
	SequenceRendererTargets.SetSelectedTarget(TargetType, (NewState == ECheckBoxState::Checked));
}

void FWidgetManager::OnOutputFormatSelectionChanged(
	TSharedPtr<FString> StringItem,
	ESelectInfo::Type SelectInfo,
	const FRendererTargetOptions::TargetType TargetType)
{
	if (*StringItem == JpegFormatName)
	{
		SequenceRendererTargets.SetOutputFormat(TargetType, EImageFormat::JPEG);
	}
	else if (*StringItem == PngFormatName)
	{
		SequenceRendererTargets.SetOutputFormat(TargetType, EImageFormat::PNG);
	}
	else if (*StringItem == ExrFormatName)
	{
		SequenceRendererTargets.SetOutputFormat(TargetType, EImageFormat::EXR);
	}
	else
	{
		UE_LOG(LogEasySynth, Error, TEXT("%s: Invalid output format selection '%s'"),
			*FString(__FUNCTION__), **StringItem);
	}
}

FText FWidgetManager::SelectedOutputFormat(const FRendererTargetOptions::TargetType TargetType) const
{
	EImageFormat OutputFormat = SequenceRendererTargets.OutputFormat(TargetType);

	if (OutputFormat == EImageFormat::JPEG)
	{
		return FText::FromString(JpegFormatName);
	}
	else if (OutputFormat == EImageFormat::PNG)
	{
		return FText::FromString(PngFormatName);
	}
	else if (OutputFormat == EImageFormat::EXR)
	{
		return FText::FromString(ExrFormatName);
	}
	else
	{
		UE_LOG(LogEasySynth, Error, TEXT("%s: Invalid target type '%d'"),
			*FString(__FUNCTION__), TargetType);
		return FText::GetEmpty();
	}
}

void FWidgetManager::OnCustomPPMaterialSelected(const FAssetData& AssetData)
{
	SequenceRendererTargets.SetCustomPPMaterialAssetData(AssetData);
}

FString FWidgetManager::GetCustomPPMaterialPath() const
{
	const auto& CustomPPMaterialAssetData = SequenceRendererTargets.CustomPPMaterial();
	if (CustomPPMaterialAssetData.IsValid())
	{
		return CustomPPMaterialAssetData.ObjectPath.ToString();
	}
	return "";
}

bool FWidgetManager::GetIsRenderImagesEnabled() const
{
	return
		!SelectedSequencesFolder.IsEmpty() &&
		SequenceRendererTargets.AnyOptionSelected() &&
		SequenceRenderer != nullptr && !SequenceRenderer->IsRendering();
}

FReply FWidgetManager::OnRenderImagesClicked()
{
	// Scan folder for level sequences using Asset Registry
	SequencesToRender.Empty();
	CurrentSequenceIndex = -1;
	
	// Convert folder path to package path
	FString PackagePath;
	if (!FPackageName::TryConvertFilenameToLongPackageName(SelectedSequencesFolder, PackagePath))
	{
		const FText MessageBoxTitle = LOCTEXT("InvalidFolderTitle", "Invalid Folder");
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("InvalidFolderMessage", "Selected folder is not a valid content folder."),
			&MessageBoxTitle);
		return FReply::Handled();
	}
	
	// Get asset registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// Find all assets in the folder
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssetsByPath(FName(*PackagePath), AssetDataList, false);
	
	// Filter for LevelSequence assets only
	for (const FAssetData& AssetData : AssetDataList)
	{
		if (AssetData.AssetClassPath.GetAssetName() == FName("LevelSequence"))
		{
			SequencesToRender.Add(AssetData);
		}
	}
	
	if (SequencesToRender.Num() == 0)
	{
		const FText MessageBoxTitle = LOCTEXT("NoSequencesFoundTitle", "No Sequences Found");
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("NoSequencesFoundMessage", "No level sequence assets found in the selected folder."),
			&MessageBoxTitle);
		return FReply::Handled();
	}
	
	// Store base output directory
	BaseOutputDirectory = OutputDirectory;
	
	// Start rendering first sequence
	CurrentSequenceIndex = 0;
	RenderCurrentSequence();
	
	// Save widget options
	SaveWidgetOptionStates();
	
	return FReply::Handled();
}

void FWidgetManager::RenderCurrentSequence()
{
	if (CurrentSequenceIndex < 0 || CurrentSequenceIndex >= SequencesToRender.Num())
	{
		return;
	}
	
	FAssetData& CurrentSequence = SequencesToRender[CurrentSequenceIndex];
	
	// Create subfolder for this sequence
	FString SequenceName = CurrentSequence.AssetName.ToString();
	FString SequenceOutputDir = BaseOutputDirectory / SequenceName;
	
	UE_LOG(LogEasySynth, Log, TEXT("Rendering sequence %d/%d: %s"), 
		CurrentSequenceIndex + 1, SequencesToRender.Num(), *SequenceName);
	
	if (!SequenceRenderer->RenderSequence(
		CurrentSequence,
		SequenceRendererTargets,
		OutputImageResolution,
		SequenceOutputDir))
	{
		const FText MessageBoxTitle = LOCTEXT("StartRenderingErrorMessageBoxTitle", "Could not start rendering");
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(SequenceRenderer->GetErrorMessage()),
			&MessageBoxTitle);
		
		// Reset batch rendering state
		SequencesToRender.Empty();
		CurrentSequenceIndex = -1;
	}
}

void FWidgetManager::OnSemanticClassesUpdated()
{
	// Refresh the list of semantic classes
	SemanticClassNames.Reset();
	TArray<FString> ClassNames = TextureStyleManager->SemanticClassNames();
	for (const FString& ClassName : ClassNames)
	{
		SemanticClassNames.Add(MakeShared<FString>(ClassName));
	}

	// Refresh the combo box
	if (SemanticClassComboBox.IsValid())
	{
		SemanticClassComboBox->RefreshOptions();
	}
	else
	{
		UE_LOG(LogEasySynth, Error, TEXT("%s: Semantic class picker is invalid, could not refresh"),
			*FString(__FUNCTION__));
	}
}

void FWidgetManager::OnRenderingFinished(bool bSuccess)
{
	if (bSuccess)
	{
		// Check if we're in batch rendering mode
		if (CurrentSequenceIndex >= 0 && CurrentSequenceIndex < SequencesToRender.Num())
		{
			CurrentSequenceIndex++;
			
			// Check if there are more sequences to render
			if (CurrentSequenceIndex < SequencesToRender.Num())
			{
				// Force memory cleanup before next sequence
				UE_LOG(LogEasySynth, Log, TEXT("Cleaning up memory before sequence %d/%d"), 
					CurrentSequenceIndex + 1, SequencesToRender.Num());
				
				// Collect garbage to free memory
				GEngine->ForceGarbageCollection(true);
				
				// Small delay before next render
				FPlatformProcess::Sleep(1.0f);
				
				// Render next sequence
				RenderCurrentSequence();
				return;
			}
			else
			{
				// All sequences rendered successfully
				const FText MessageBoxTitle = LOCTEXT("BatchRenderingCompleteTitle", "Batch Rendering Complete");
				FMessageDialog::Open(
					EAppMsgType::Ok,
					FText::Format(LOCTEXT("BatchRenderingCompleteMessage", "Successfully rendered {0} sequences."),
						FText::AsNumber(SequencesToRender.Num())),
					&MessageBoxTitle);
				
				// Reset batch rendering state
				SequencesToRender.Empty();
				CurrentSequenceIndex = -1;
			}
		}
		else
		{
			// Single sequence rendering (shouldn't happen now, but keep for safety)
			const FText MessageBoxTitle = LOCTEXT("SuccessfulRenderingMessageBoxTitle", "Successful rendering");
			FMessageDialog::Open(
				EAppMsgType::Ok,
				LOCTEXT("SuccessfulRenderingMessageBoxText", "Rendering finished successfully"),
				&MessageBoxTitle);
		}
	}
	else
	{
		// Error occurred
		const FText MessageBoxTitle = LOCTEXT("RenderingErrorMessageBoxTitle", "Rendering failed");
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(FString::Printf(TEXT("Failed on sequence: %s\n\nError: %s"),
				CurrentSequenceIndex >= 0 && CurrentSequenceIndex < SequencesToRender.Num() 
					? *SequencesToRender[CurrentSequenceIndex].AssetName.ToString() 
					: TEXT("Unknown"),
				*SequenceRenderer->GetErrorMessage())),
			&MessageBoxTitle);
		
		// Reset batch rendering state
		SequencesToRender.Empty();
		CurrentSequenceIndex = -1;
	}
}

void FWidgetManager::LoadWidgetOptionStates()
{
	// Try to load
	UWidgetStateAsset* WidgetStateAsset = LoadObject<UWidgetStateAsset>(nullptr, *FPathUtils::WidgetStateAssetPath());

	if (WidgetStateAsset != nullptr)
	{
		// Initialize the widget members using loaded options
		SelectedSequencesFolder = TEXT("");
		SequenceRendererTargets.SetExportCameraPoses(WidgetStateAsset->bCameraPosesSelected);
		SequenceRendererTargets.SetSelectedTarget(FRendererTargetOptions::COLOR_IMAGE, WidgetStateAsset->bColorImagesSelected);
		SequenceRendererTargets.SetSelectedTarget(FRendererTargetOptions::DEPTH_IMAGE, WidgetStateAsset->bDepthImagesSelected);
		SequenceRendererTargets.SetSelectedTarget(FRendererTargetOptions::NORMAL_IMAGE, WidgetStateAsset->bNormalImagesSelected);
		SequenceRendererTargets.SetSelectedTarget(FRendererTargetOptions::OPTICAL_FLOW_IMAGE, WidgetStateAsset->bOpticalFlowImagesSelected);
		SequenceRendererTargets.SetSelectedTarget(FRendererTargetOptions::SEMANTIC_IMAGE, WidgetStateAsset->bSemanticImagesSelected);
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::COLOR_IMAGE,
			static_cast<EImageFormat>(WidgetStateAsset->bColorImagesOutputFormat));
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::DEPTH_IMAGE,
			static_cast<EImageFormat>(WidgetStateAsset->bDepthImagesOutputFormat));
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::NORMAL_IMAGE,
			static_cast<EImageFormat>(WidgetStateAsset->bNormalImagesOutputFormat));
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::OPTICAL_FLOW_IMAGE,
			static_cast<EImageFormat>(WidgetStateAsset->bOpticalFlowImagesOutputFormat));
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::SEMANTIC_IMAGE,
			static_cast<EImageFormat>(WidgetStateAsset->bSemanticImagesOutputFormat));
		SequenceRendererTargets.SetCustomPPMaterialAssetData(WidgetStateAsset->CustomPPMaterialAssetPath.TryLoad());
		SequenceRendererTargets.SetOutputFormat(
			FRendererTargetOptions::CUSTOM_PP_MATERIAL,
			static_cast<EImageFormat>(WidgetStateAsset->bCustomPPMaterialOutputFormat));
		OutputImageResolution = WidgetStateAsset->OutputImageResolution;
		SequenceRendererTargets.SetDepthRangeMeters(WidgetStateAsset->DepthRange);
		SequenceRendererTargets.SetOpticalFlowScale(WidgetStateAsset->OpticalFlowScale);
		OutputDirectory = WidgetStateAsset->OutputDirectory;
	}
}

void FWidgetManager::SaveWidgetOptionStates()
{
	// Load the asset, or create new one if it does not exist
	UWidgetStateAsset* WidgetStateAsset = LoadObject<UWidgetStateAsset>(nullptr, *FPathUtils::WidgetStateAssetPath());
	if (WidgetStateAsset == nullptr)
	{
		UE_LOG(LogEasySynth, Log, TEXT("%s: Texture mapping asset not found, creating a new one"),
			*FString(__FUNCTION__));

		// Register the plugin directory with the editor
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		AssetRegistryModule.Get().AddPath(FPathUtils::ProjectPluginContentDir());

		// Create and populate the asset
		UPackage *WidgetStatePackage = CreatePackage(*FPathUtils::WidgetStateAssetPath());
		check(WidgetStatePackage)
		WidgetStateAsset = NewObject<UWidgetStateAsset>(
			WidgetStatePackage,
			UWidgetStateAsset::StaticClass(),
			*FPathUtils::WidgetStateAssetName,
			EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
		check(WidgetStateAsset)
	}

	// Update asset values
	WidgetStateAsset->LevelSequenceAssetPath = FSoftObjectPath();
	WidgetStateAsset->bCameraPosesSelected = SequenceRendererTargets.ExportCameraPoses();
	WidgetStateAsset->bColorImagesSelected = SequenceRendererTargets.TargetSelected(FRendererTargetOptions::COLOR_IMAGE);
	WidgetStateAsset->bDepthImagesSelected = SequenceRendererTargets.TargetSelected(FRendererTargetOptions::DEPTH_IMAGE);
	WidgetStateAsset->bNormalImagesSelected = SequenceRendererTargets.TargetSelected(FRendererTargetOptions::NORMAL_IMAGE);
	WidgetStateAsset->bOpticalFlowImagesSelected = SequenceRendererTargets.TargetSelected(FRendererTargetOptions::OPTICAL_FLOW_IMAGE);
	WidgetStateAsset->bSemanticImagesSelected = SequenceRendererTargets.TargetSelected(FRendererTargetOptions::SEMANTIC_IMAGE);
	WidgetStateAsset->bColorImagesOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::COLOR_IMAGE));
	WidgetStateAsset->bDepthImagesOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::DEPTH_IMAGE));
	WidgetStateAsset->bNormalImagesOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::NORMAL_IMAGE));
	WidgetStateAsset->bOpticalFlowImagesOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::OPTICAL_FLOW_IMAGE));
	WidgetStateAsset->bSemanticImagesOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::SEMANTIC_IMAGE));
	WidgetStateAsset->CustomPPMaterialAssetPath = SequenceRendererTargets.CustomPPMaterial().ToSoftObjectPath();
	WidgetStateAsset->bCustomPPMaterialOutputFormat = static_cast<int8>(
		SequenceRendererTargets.OutputFormat(FRendererTargetOptions::CUSTOM_PP_MATERIAL));
	WidgetStateAsset->OutputImageResolution = OutputImageResolution;
	WidgetStateAsset->DepthRange = SequenceRendererTargets.DepthRangeMeters();
	WidgetStateAsset->OpticalFlowScale = SequenceRendererTargets.OpticalFlowScale();
	WidgetStateAsset->OutputDirectory = OutputDirectory;

	// Save the asset
	const bool bOnlyIfIsDirty = false;
	UEditorAssetLibrary::SaveLoadedAsset(WidgetStateAsset, bOnlyIfIsDirty);
}

#undef LOCTEXT_NAMESPACE
