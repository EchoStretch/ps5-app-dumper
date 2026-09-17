async function main() {
    const PAYLOAD = window.workingDir + '/ps5-app-dumper.elf';

    return {
        mainText: "PS5 App Dumper",
        secondaryText: 'Web UI - dump apps to USB',
	onclick: async () => {
	    return {
		path: PAYLOAD,
                daemon: true
	    };
        }
    };
}
