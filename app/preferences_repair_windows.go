//go:build windows

package main

import "path/filepath"

func offerPreferencesRepair(path string, cause error) error {
	if msgBox(uiTextWith("preferences.repair.confirm", map[string]string{"file": filepath.Base(path), "error": cause.Error()}), mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return errSetupCancelled
	}
	saved, err := repairPreferences(path)
	if err != nil {
		return err
	}
	infoBox(uiTextWith("preferences.repair.done", map[string]string{"path": saved}))
	return nil
}

func loadSettingsWithRepair(path string) (settings, error) {
	value, err := loadSettings(path)
	if err == nil {
		return value, nil
	}
	if err = offerPreferencesRepair(path, err); err != nil {
		return settings{}, err
	}
	return loadSettings(path)
}
func loadStorageWithRepair(dir string) (storageSettings, error) {
	value, err := loadStorageSettings(dir)
	if err == nil {
		return value, nil
	}
	if err = offerPreferencesRepair(filepath.Join(dir, storageSettingsFilename), err); err != nil {
		return storageSettings{}, err
	}
	return loadStorageSettings(dir)
}
