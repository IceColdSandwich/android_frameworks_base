package com.android.systemui.statusbar.policy.toggles;

import android.provider.Settings;
import android.content.Context;

import com.android.systemui.R;

public class BlnToggle extends Toggle {
	

    public BlnToggle(Context context) {
        super(context);

        updateState();
        setLabel(R.string.toggle_bln);
        if (mToggle.isChecked())
        	setIcon(R.drawable.toggle_bln);
        else
        	setIcon(R.drawable.toggle_bln_off);
        
    }

    @Override
    protected void updateInternalToggleState() {
    	mToggle.setChecked(isBlnOn());
        if (mToggle.isChecked())
        	setIcon(R.drawable.toggle_bln);
        else
        	setIcon(R.drawable.toggle_bln_off);
    }

    @Override
    protected void onCheckChanged(boolean isChecked) {
    	updateBln(isChecked);
        if (isChecked)
        	setIcon(R.drawable.toggle_bln);
        else
        	setIcon(R.drawable.toggle_bln_off);
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
