LOCAL_PATH:= $(call my-dir) 
 include $(CLEAR_VARS)
 LOCAL_SRC_FILES:= \
     backupcar_main.cpp \
     BackupVideo.cpp \
     BackupCar.cpp
 LOCAL_CFLAGS += -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES
 LOCAL_SHARED_LIBRARIES := \
     libcutils \
     liblog \
     libandroidfw \
     libutils \
     libbinder \
     libui \
     libskia \
     libEGL \
     libGLESv1_CM \
     libgui
 
 LOCAL_C_INCLUDES := \
     $(call include-path-for, corecg graphics)
 
 LOCAL_MODULE:= backupcar 
 include $(BUILD_EXECUTABLE)
 
 include $(CLEAR_VARS)
  LOCAL_SRC_FILES:= live.cpp \
                surface_video.cpp \
                video_capture.cpp
  LOCAL_SHARED_LIBRARIES := \
      libcutils \
      liblog \
      libandroidfw \
      libutils \
      libbinder \
      libui \
      libEGL \
      libGLESv2 \
      libGLESv1_CM \
      libgui
 
 LOCAL_MODULE:= live
 include $(BUILD_EXECUTABLE)
