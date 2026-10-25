## How to run on Windows:
* Download the latest release.
* Download [extract-xiso](https://github.com/xboxdev/extract-xiso) to extract game files from an ISO.
* Download [god2iso](https://github.com/raburton/god2iso) (necessary for a dump from an actual disk)
* Run god2iso and convert the Game On Demand (god) to an ISO file.
* Run extract-xiso and save the game files to a safe directory.
* Open the ``run.bat`` file in the PGR4 Recomp area.
* Edit the path to the game data root in the ``run.bat``, as this is where the command line arguments are kept, and save changes.
* Run the ``run.bat``
* The game should launch!

## How to run on SteamOS:
* Go into Desktop Mode
* Follow the same extracting game file steps as Windows, you may need to use Proton to get them to work.
* Add ``pgr4_launcher.exe`` to Steam, do not remove it from the parent directory with ``pgr4_recompiled.exe`` else the launcher will NOT work, this handles all of the launch arguments, and even has controller support!
* Set the compatibility tool to Proton Experimental, this works best with the recomp.
* Launch the launcher, this includes all adjustment features you need.
* Once ready, press "Play" and enjoy!

### Please Note:
Steps are due to change as this project evolves, these are just steps for how to get it up and running on Windows and SteamOS, other operating systems will have steps placed in here once their builds are confirmed working.
