async function main() {
    const PAYLOAD = window.workingDir + '/ps5-app-dumper.elf';

    return {
        mainText: "PS5 App Dumper",
        secondaryText: 'Dump PS5 App To USB',
	onclick: async () => {
	    return {
		path: PAYLOAD,
                daemon: true
	    };
        }
    };
}
