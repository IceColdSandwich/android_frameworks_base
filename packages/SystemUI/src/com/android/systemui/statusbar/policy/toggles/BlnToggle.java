package com.android.systemui.statusbar.policy.toggles;

import android.provider.Settings;
import android.content.ContentResolver;
import android.database.ContentObserver;
import android.os.Handler;
import android.content.Context;

import com.android.systemui.R;

public class BlnToggle extends Toggle {
	

    public BlnToggle(Context context) {
        super(context);
        SettingsObserver obs = new SettingsObserver(new Handler());
        obs.observe();
        setLabel(R.string.toggle_bln);
        updateState();
    }

    class SettingsObserver extends ContentObserver {
        SettingsObserver(Handler handler) {
            super(handler);
        }

        void observe() {
            ContentResolver resolver = mContext.getContentResolver();
            resolver.registerContentObserver(Settings.Secure.
                    getUriFor(Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT), false,
                    this);
            updateState();
        }

        @Override
        public void onChange(boolean selfChange) {
            updateState();
        }
    }

    @Override
    protected boolean updateInternalToggleState() {
    	mToggle.setChecked(isBlnOn());
        if (mToggle.isChecked())
        	setIcon(R.drawable.toggle_bln);
        else
        	setIcon(R.drawable.toggle_bln_off);
        return mToggle.isChecked();
    }

    @Override
    protected void onCheckChanged(boolean isChecked) {
    	updateBln(isChecked);
        if (isChecked)
        	setIcon(R.drawable.toggle_bln);
        else
        	setIcon(R.drawable.toggle_bln_off);
        updateState();
    }
    
    @Override
    protected boolean onLongPress() {
       return false;
    }
    
    public boolean isBlnOn(){
    	return Settings.System.getInt(mContext.getContentResolver(),
                            Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT, 0) != 0;
    }
    
    public void updateBln(boolean on){
    	Settings.System.putInt(mContext.getContentResolver(),
                            Settings.System.NOTIFICATION_USE_BUTTON_BACKLIGHT, on ? 1:0);
    	
    }
}
