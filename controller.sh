
#!/bin/bash

# Author: Madhav Appanaboyina
# Brief: Interactive dashboard/interface for user friendly inputs and tracking the state of control commands.
# --- Configuration ---
PI_IP="10.42.0.197"
PORT="9000"

# --- Local State Tracking (Defaults matching C++ backend) ---
CURRENT_TEMPO="1.0"
CURRENT_PITCH="1.0"
LOOP_A="0.0"
LOOP_B="End"
LOOP_STATUS="OFF"
PLAY_STATUS="PLAYING"

# --- Main Application Loop ---
while true; do
    clear
    echo "======================================"
    echo "  AESD Audio Controller (Port 9000)   "
    echo "======================================"
    echo " Current Audio State:"
    echo "   Playback : $PLAY_STATUS"
    echo "   Tempo    : ${CURRENT_TEMPO}x"
    echo "   Pitch    : ${CURRENT_PITCH}x"
    echo "   Looping  : $LOOP_STATUS (A: $LOOP_A | B: $LOOP_B)"
    echo "--------------------------------------"
    echo " Available Commands:"
    echo "   tempo <val>   (e.g., tempo 1.5)"
    echo "   pitch <val>   (e.g., pitch 0.8)"
    echo "   loop <A> <B>  (e.g., loop 5.0 10.0)"
    echo "   loop off      (Disables looping)"
    echo "   pause         (Pauses audio)"
    echo "   play          (Resumes audio)"
    echo "   exit          (Closes controller)"
    echo "======================================"

    # Prompt user for input
    read -p "Enter command: " cmd arg1 arg2

    # Parse input and send commands via netcat
    case $cmd in
        tempo)
            # 1. ALWAYS send the command to the Pi to trigger backend C++ logs
            echo "set tempo $arg1" | nc -w 1 $PI_IP $PORT
            
            # 2. Local bounds check strictly to protect UI dashboard accuracy
            is_valid=$(awk -v val="$arg1" 'BEGIN {print (val >= 0.5 && val <= 2.0)}')
            if [ "$is_valid" -eq 1 ]; then
                CURRENT_TEMPO=$arg1
            else
                echo "UI Info: Invalid tempo sent. Check Pi terminal for backend rejection."
                sleep 2
            fi
            ;;
        pitch)
            # 1. ALWAYS send the command to the Pi to trigger backend C++ logs
            echo "set pitch $arg1" | nc -w 1 $PI_IP $PORT
            
            # 2. Local bounds check strictly to protect UI dashboard accuracy
            is_valid=$(awk -v val="$arg1" 'BEGIN {print (val >= 0.5 && val <= 2.0)}')
            if [ "$is_valid" -eq 1 ]; then
                CURRENT_PITCH=$arg1
            else
                echo "UI Info: Invalid pitch sent. Check Pi terminal for backend rejection."
                sleep 2
            fi
            ;;
        loop)
            if [ "$arg1" == "off" ]; then
                echo "set loop_off 1.0" | nc -w 1 $PI_IP $PORT
                LOOP_STATUS="OFF"
            else
                # 1. ALWAYS send atomic command to Pi to trigger backend C++ logs
                echo "set loop $arg1 $arg2" | nc -w 1 $PI_IP $PORT
                
                # 2. Local UI sanity check (A >= 0 and A < B)
                is_valid=$(awk -v a="$arg1" -v b="$arg2" 'BEGIN {print (a >= 0 && a < b)}')
                
                if [ "$is_valid" -eq 1 ]; then
                    LOOP_A=$arg1
                    LOOP_B=$arg2
                    LOOP_STATUS="ON"
                else
                    echo "UI Info: Invalid loop bounds sent. Check Pi terminal for backend rejection."
                    sleep 2
                fi
            fi
            ;;
        pause)
            echo "set pause 1.0" | nc -w 1 $PI_IP $PORT
            PLAY_STATUS="PAUSED"
            ;;
        play)
            echo "set pause 0.0" | nc -w 1 $PI_IP $PORT
            PLAY_STATUS="PLAYING"
            ;;
        exit)
            clear
            echo "Sending kill signal to Audio Engine"
            # This triggers the graceful shutdown in the C++ backend
            echo "set quit 1.0" | nc -w 1 $PI_IP $PORT
            echo "Exiting Controller..."
            break
            ;;
        *)
            echo "Invalid command format."
            sleep 1
            ;;
    esac
done


