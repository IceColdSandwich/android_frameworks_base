package com.android.systemui.statusbar.powerwidget;

import com.android.systemui.R;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.Settings;

import java.util.ArrayList;
import java.util.List;

public class BlnButton extends PowerButton {

    private static final List<Uri> OBSERVED_URIS = new ArrayList<Uri>();
    static {
        OBSERVED_URIS.add(Settings.System.getUriFor(Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT));
    }

    public BlnButton() { mType = BUTTON_BLN; }

    @Override
    protected void updateState() {
        if (getBlnState(mView.getContext()) == 1) {
            mIcon = R.drawable.stat_bln_on;
            mState = STATE_ENABLED;
        } else {
            mIcon = R.drawable.stat_bln_off;
            mState = STATE_DISABLED;
        }
    }

    @Override
    protected void toggleState() {
        Context context = mView.getContext();
        if(getBlnState(context) == 0) {
            Settings.System.putInt(
                    context.getContentResolver(),
                    Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT, 1);
        } else {
            Settings.System.putInt(
                    context.getContentResolver(),
                    Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT, 0);
        }
    }


    @Override
    protected boolean handleLongClick() {
	// not working this way     
	/*
	Intent intent = new Intent();
	intent.setClassName("com.android.settings", "com.android.settings.RomCustomSettings");
    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    mView.getContext().startActivity(intent);
    return true;
	*/
    return false;
    }
	

    @Override
    protected List<Uri> getObservedUris() {
        return OBSERVED_URIS;
    }

    private static int getBlnState(Context context) {
        return Settings.System.getInt(
                context.getContentResolver(),
                Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT, 0);
    }
}
